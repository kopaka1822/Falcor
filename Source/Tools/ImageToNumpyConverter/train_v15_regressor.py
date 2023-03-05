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
from shared import preprocess_ray_labels
import pickle
from keras.callbacks import EarlyStopping
from keras.layers import LeakyReLU

NUM_SAMPLES = 8
CLEAR_FILE = False # clears cached files

BATCH_SIZE = 1024

LAYERS = [16, 16]
#ML_NAME = f"net_{LAYERS}_{NEURONS}_"
ML_NAME = f"net_relu_reg"

# set current directory as working directory
os.chdir(os.path.dirname(os.path.abspath(__file__)))

ACCURACY_LOG = f'accuracy_log.txt'

def build_net_relu_ex():

	input_raster = keras.Input(shape=(8, ))
	input_asked = keras.Input(shape=(8, ))
	
	prev_layer = input_raster
	for layer_size in LAYERS:
		prev_layer = keras.layers.Dense(layer_size, activation="relu", kernel_initializer="he_normal")(prev_layer)


	# final layer for multi-label classification (8 labels)
	#classify = keras.layers.Dense(8, activation=lambda x: tf.clip_by_value(x, clip_value_min=-1, clip_value_max=1))(prev_layer)
	classify = keras.layers.Dense(8, activation="linear")(prev_layer)
	#classify = keras.layers.Dense(8, activation="tanh")(prev_layer)

	multiply = keras.layers.Multiply()([classify, input_asked])

	model = keras.models.Model(inputs=[input_raster, input_asked], outputs=multiply)

	opt = tf.keras.optimizers.Nadam(learning_rate=0.003)
	model.compile(optimizer=opt, loss='mean_absolute_error', metrics=[tf.keras.metrics.MeanAbsoluteError(), tf.keras.metrics.MeanSquaredError()], run_eagerly=False)
	return model

def print_stats(clf, rasterf, rayf, askedf, sphereStart, sphereEnd):
	# predict test data
	#y_pred = clf.predict(rasterf).reshape(-1)
	y_pred = clf.predict(rasterf, batch_size=BATCH_SIZE).reshape(-1)
	# clip values (if they were not clipped by the net)
	y_pred = np.clip(y_pred, -1, 1)

	asked_flat = askedf.reshape(-1)
	# only take entries where asked is 1
	y_pred = y_pred.reshape(-1)[asked_flat == 1]
	rayf = rayf.reshape(-1)[asked_flat == 1]
	sphereStart = sphereStart.reshape(-1)[asked_flat == 1]
	sphereEnd = sphereEnd.reshape(-1)[asked_flat == 1]

	print("Median Height Error: ", np.median(np.abs(y_pred - rayf)))

	# calc ao based on sphere start and end
	ao_pred = np.maximum(sphereStart - np.maximum(sphereEnd, y_pred), 0.0) / (2.0 * sphereStart)
	ao_true = np.maximum(sphereStart - np.maximum(sphereEnd, rayf), 0.0) / (2.0 * sphereStart)
	print("Mean AO Error: ", np.mean(np.abs(ao_pred - ao_true)))

def write_weights(clf):
	weights = clf.get_weights()
	#print_weights(weights)
	layers = len(weights) // 2
	for l in range(layers):
		np.save(f'eval/{ML_NAME}_weights0_kernel{l}.npy', weights[2 * l + 0])
		np.save(f'eval/{ML_NAME}_weights0_bias{l}.npy', weights[2 * l + 1])

def inspectSample():
	tf.keras.utils.set_random_seed(1) # use same random seed for training
	filename = f"{ML_NAME}.pkl"

	rasterf = np.load(f'train/raster_train_.npy')
	askedf = np.load(f'train/asked_train_.npy')
	rayf = np.load(f'train/ray_train_.npy')
	sphere_startf = np.load(f'train/sphere_start_train_.npy')
	sphere_endf = np.load(f'train/sphere_end_train_.npy')

	raster_validationf = np.load(f'eval/raster_eval_0.npy')
	asked_validationf = np.load(f'eval/asked_eval_0.npy')
	ray_validationf = np.load(f'eval/ray_eval_0.npy')
	sphere_start_validationf = np.load(f'eval/sphere_start_eval_0.npy')
	sphere_end_validationf = np.load(f'eval/sphere_end_eval_0.npy')

	# preprocess data
	print("preprocessing data")
	rasterf = preprocess_inputs_vao(rasterf, askedf)
	rayf = preprocess_ray_labels(rayf, askedf)
	raster_validationf = preprocess_inputs_vao(raster_validationf, asked_validationf)
	ray_validationf = preprocess_ray_labels(ray_validationf, asked_validationf)

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

		clf.fit(rasterf, rayf, 
			epochs=1000,
			validation_data=(raster_validationf, ray_validationf), 
			verbose=1, 
			batch_size=BATCH_SIZE, #8192
			#net__class_weight=class_weight,
			callbacks=[EarlyStopping(monitor='mean_squared_error', patience=10, min_delta=0.0001, start_from_epoch=2)]
		)
		# also save in file
		pickle.dump(clf, open(filename, "wb"))

	# predict test data
	# print weights
	write_weights(clf)

	weights = clf.get_weights()
	for i in range(len(weights)):
		print("weights", i, ":", np.round(weights[i], 2))
	
	print("------- original test -------")
	print_stats(clf, rasterf, rayf, askedf, sphere_startf, sphere_endf)

	print("------- original valid -------")
	print_stats(clf, raster_validationf, ray_validationf, asked_validationf, sphere_start_validationf, sphere_end_validationf)

	print("----------------------------------------------------------------")



inspectSample()