# idea: train neural network with multi-label classification

import numpy as np
import os
import matplotlib.pyplot as plt
from sklearn.model_selection import cross_val_score
from sklearn.linear_model import LogisticRegression
from sklearn.metrics import confusion_matrix
from sklearn.metrics import f1_score
from sklearn.preprocessing import StandardScaler
from sklearn.preprocessing import RobustScaler
from sklearn.pipeline import Pipeline
from sklearn import tree
from sklearn.model_selection import train_test_split
from sklearn.ensemble import RandomForestClassifier
from sklearn.model_selection import GridSearchCV
from sklearn.preprocessing import PolynomialFeatures
from sklearn.svm import LinearSVC
from sklearn.ensemble import BaggingClassifier
from sklearn.ensemble import AdaBoostClassifier
from sklearn.model_selection import RandomizedSearchCV
import tensorflow as tf
from tensorflow import keras
from shared import preprocess_inputs_vao
import pickle
from keras.callbacks import EarlyStopping
from keras.layers import LeakyReLU

NUM_SAMPLES = 8
CLEAR_FILE = True # clears cached files

BATCH_SIZE = 1024

LAYERS = 1
NEURONS = 8
#ML_NAME = f"net_{LAYERS}_{NEURONS}_"
ML_NAME = f"net_relu"

# set current directory as working directory
os.chdir(os.path.dirname(os.path.abspath(__file__)))

ACCURACY_LOG = f'accuracy_log.txt'

def build_net_relu_ex():

	input_raster = keras.Input(shape=(8, ))
	input_asked = keras.Input(shape=(8, ))
	
	prev_layer = input_raster
	for layer in range(LAYERS):
		prev_layer = keras.layers.Dense(NEURONS, activation="relu", kernel_initializer="he_normal")(prev_layer)

	# final layer for multi-label classification (8 labels)
	classify = keras.layers.Dense(8, activation="sigmoid")(prev_layer)
	output = keras.layers.Multiply()([classify, input_asked])

	model = keras.models.Model(inputs=[input_raster, input_asked], outputs=output)

	opt = tf.keras.optimizers.Nadam(learning_rate=0.003)
	model.compile(optimizer=opt, loss='binary_crossentropy', metrics=[tf.keras.metrics.BinaryAccuracy()], run_eagerly=False)
	return model

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

def inspectSample():
	tf.keras.utils.set_random_seed(1) # use same random seed for training
	filename = f"{ML_NAME}.pkl"

	rasterf = np.load(f'train/raster_train_.npy')
	askedf = np.load(f'train/asked_train_.npy')
	requiredf = np.load(f'train/required_train_.npy')

	raster_validationf = np.load(f'eval/raster_eval_0.npy')
	required_validationf = np.load(f'eval/required_eval_0.npy')
	asked_validationf = np.load(f'eval/asked_eval_0.npy')

	# TODO class weights?
	#numClass1 = np.count_nonzero(requiredf)
	#numClass0 = len(requiredf) - numClass1
	#class_weight = {0: max(numClass1/numClass0, 1.0), 1: max(numClass0/numClass1, 1.0)}

	# preprocess data
	print("preprocessing data")
	rasterf = preprocess_inputs_vao(rasterf, askedf)
	raster_validationf = preprocess_inputs_vao(raster_validationf, asked_validationf)

	print("length of data: ", len(rasterf))

	clf = None
	try:
		if CLEAR_FILE and os.path.exists(filename):
			os.remove(filename)

		clf = pickle.load(open(filename, 'rb'))
	except:
		print('no saved model found -> training new model')
		net = build_net_relu_ex()
		
		clf = net

		clf.fit(rasterf, requiredf, 
			epochs=1000,
			#net__validation_data=(raster_validationf, required_validationf), 
			verbose=1, 
			batch_size=BATCH_SIZE, #8192
			#net__class_weight=class_weight,
			callbacks=[EarlyStopping(monitor='binary_accuracy', patience=10, min_delta=0.0001, start_from_epoch=2)]
		)
		# also save in file
		pickle.dump(clf, open(filename, "wb"))

	# predict test data
	# print weights
	weights = clf.get_weights()
	for i in range(len(weights)):
		print("weights", i, ":", np.round(weights[i], 2))
	
	print("------- original test -------")
	print_stats(clf, rasterf, requiredf, askedf)

	print("------- original valid -------")
	print_stats(clf, raster_validationf, required_validationf, asked_validationf)

	print("----------------------------------------------------------------")



inspectSample()