# idea: take all 32 samples but train only 4 estimators (one for each step)

import numpy as np
import os
import matplotlib.pyplot as plt
from sklearn.model_selection import cross_val_score
from sklearn.linear_model import LogisticRegression
from sklearn.metrics import confusion_matrix
from sklearn.metrics import f1_score
from sklearn.preprocessing import StandardScaler
from sklearn.pipeline import Pipeline
from sklearn import tree
from sklearn.model_selection import train_test_split
from sklearn.ensemble import RandomForestClassifier
import pickle
from shared import preprocess_inputs_vao
import tensorflow_model_optimization as tfmot

import tensorflow as tf
from tensorflow import keras

NUM_SAMPLES = 8
EVAL_ID = 0
ML_NAME = "net_relu"
BATCH_SIZE = 1024

# set current directory as working directory
os.chdir(os.path.dirname(os.path.abspath(__file__)))

def print_stats(clf, rasterf, requiredf, askedf):
	# predict test data
	#y_pred = clf.predict(rasterf).reshape(-1)
	y_pred = clf.predict(rasterf, batch_size=BATCH_SIZE).reshape(-1)
	y_pred = np.where(y_pred > 0.5, 1, 0)

	requiredf_flat = requiredf.reshape(-1)

	asked_flat = askedf.reshape(-1)

	acc = np.count_nonzero(y_pred == requiredf_flat) / len(y_pred)
	print("original accuracy: ", acc)
	
	# filter out unasked
	y_pred = y_pred[asked_flat == 1]
	requiredf_flat = requiredf_flat[asked_flat == 1]

	# print test set stats
	acc = np.count_nonzero(y_pred == requiredf_flat) / len(y_pred)
	print("filtered accuracy: ", acc)
	print("confusion matrix:")
	cm = confusion_matrix(requiredf_flat, y_pred)
	print(cm)
	f1 = f1_score(requiredf_flat, y_pred)
	print("F1 score: ", f1)
	print("Recall: ", cm[1][1] / (cm[1][1] + cm[1][0]))
	return acc

def prunemodel():
	tf.keras.utils.set_random_seed(1) # use same random seed for training
	filename = f"{ML_NAME}.pkl"

	rasterf = np.load(f'train/raster_train_.npy')
	askedf = np.load(f'train/asked_train_.npy')
	requiredf = np.load(f'train/required_train_.npy')

	raster_validationf = np.load(f'eval/raster_eval_0.npy')
	required_validationf = np.load(f'eval/required_eval_0.npy')
	asked_validationf = np.load(f'eval/asked_eval_0.npy')

	# preprocess data
	print("preprocessing data")
	rasterf = preprocess_inputs_vao(rasterf, askedf)
	raster_validationf = preprocess_inputs_vao(raster_validationf, asked_validationf)

	clf = pickle.load(open(filename, 'rb'))
	clf.summary()
	print(clf.get_weights())
	original_acc = print_stats(clf, raster_validationf, required_validationf, asked_validationf)

	# try different target sparsity
	# do binary search to find optimal sparsity
	low = 0.0
	high = 1.0
	models = {}
	results = {}
	sparsities = []
	for i in range(10): # do binary search 10 times
		sparsity = (low + high) / 2
		sparsities.append(sparsity)
		print(f"--------------> testing sparsity {sparsity} ({i+1}/10) <----------------")

		# train
		epochs = 2
		pruning_params = {
			#'pruning_schedule': tfmot.sparsity.keras.PolynomialDecay(initial_sparsity=0.50, final_sparsity=0.80, begin_step=0, end_step=(epochs * np.ceil(len(rasterf) / BATCH_SIZE).astype(np.int32)))
			'pruning_schedule': tfmot.sparsity.keras.ConstantSparsity(target_sparsity=sparsity, begin_step=0)
		}

		prune_clf = tfmot.sparsity.keras.prune_low_magnitude(clf, **pruning_params)
		prune_clf.compile(optimizer='adam', loss='binary_crossentropy', metrics=[tf.keras.metrics.BinaryAccuracy()])

		prune_clf.fit(rasterf, requiredf, epochs=epochs, batch_size=BATCH_SIZE, validation_data=(raster_validationf, required_validationf), callbacks=[tfmot.sparsity.keras.UpdatePruningStep()])
		export_model = tfmot.sparsity.keras.strip_pruning(prune_clf)
		new_acc = print_stats(export_model, raster_validationf, 	required_validationf, asked_validationf)
		results[sparsity] = new_acc
		models[sparsity] = export_model

		# if accuary is too low, decrease sparsity
		if new_acc < 0.99 * original_acc:
			high = sparsity
		else:
			low = sparsity

	print("original accuracy: ", original_acc)
	print(results)
	# get best model (highest 99% of accuracy with highest sparsity)
	best_sparsity = 0
	best_acc = 0
	for sparsity in sparsities:
		if results[sparsity] >= 0.99 * best_acc:
			best_acc = max(results[sparsity], best_acc)
			best_sparsity = sparsity
	
	print("best sparsity: ", best_sparsity)
	export_model = models[best_sparsity]



	# transfer weights
	print(export_model.get_weights())
	clf.set_weights(export_model.get_weights())
	pickle.dump(clf, open(f"{ML_NAME}_pruned.pkl", 'wb'))


prunemodel()