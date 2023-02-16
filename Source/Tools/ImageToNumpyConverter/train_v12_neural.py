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
from shared import preprocess_inputs
import pickle
from keras.callbacks import EarlyStopping
from keras.layers import LeakyReLU

NUM_DIRECTIONS = 8
NUM_STEPS = 4
NUM_SAMPLES = NUM_DIRECTIONS * NUM_STEPS
CLEAR_FILE = True # clears cached files

BATCH_SIZE = 1024

LAYERS = 1
NEURONS = 8
#ML_NAME = f"net_{LAYERS}_{NEURONS}_"
ML_NAME = f"net_relu"

# set current directory as working directory
os.chdir(os.path.dirname(os.path.abspath(__file__)))

# hide gpu for cpu usage
#tf.config.set_visible_devices([], 'GPU')
#tf.keras.mixed_precision.set_global_policy('mixed_float16')

ACCURACY_LOG = f'accuracy_log.txt'

def build_net_relu_ex(n_hidden = LAYERS, n_neurons = NEURONS):

	input_raster = keras.Input(shape=(5, ))
	input_asked = keras.Input(shape=(4, ))
	
	prev_layer = input_raster
	for layer in range(n_hidden):
		prev_layer = keras.layers.Dense(n_neurons, activation="relu", kernel_initializer="he_normal")(prev_layer)

	# final layer for multi-label classification (4 labels)
	classify = keras.layers.Dense(4, activation="sigmoid")(prev_layer)
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
	rasterf = preprocess_inputs(rasterf, askedf)
	raster_validationf = preprocess_inputs(raster_validationf, asked_validationf)

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
	#print("------- refined test -------")
	#print_stats(clf2, rasterf, requiredf)

	print("------- original valid -------")
	print_stats(clf, raster_validationf, required_validationf, asked_validationf)

	'''[[ 703510   68179] training ref 0.8696727055933129
 	   [ 228246 1274531]]'''
	'''[[ 50271   3182]  validation ref 0.8901166274192874
 	    [ 20476 141372]]'''


	

	print("----------------------------------------------------------------")
	#tree.plot_tree(clf)
	#plt.show()


#inspectSample(3)

#for i in range(NUM_STEPS):
#for rng_seed in range(4):
inspectSample()

#with open(ACCURACY_LOG, 'a') as f:
#	f.write('----------------------------------------------------------------\n')
