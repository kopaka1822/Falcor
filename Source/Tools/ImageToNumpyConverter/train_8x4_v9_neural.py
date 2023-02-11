# idea: train logistic regression on all 32 samples

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
from shared import ClampTransformer
from shared import FilterInputsTransformer
import pickle
from keras.callbacks import EarlyStopping
from keras.layers import LeakyReLU

NUM_DIRECTIONS = 8
NUM_STEPS = 4
NUM_SAMPLES = NUM_DIRECTIONS * NUM_STEPS
CLEAR_FILE = True # clears cached files

LAYERS = 2
NEURONS = 5
#ML_NAME = f"net_{LAYERS}_{NEURONS}_"
ML_NAME = f"net_relu"
ML_REFINED = f"{ML_NAME}_refined"

# set current directory as working directory
os.chdir(os.path.dirname(os.path.abspath(__file__)))

print("TensorFlow version: ", tf.__version__)
print("Num GPUs Available: ", len(tf.config.list_physical_devices('GPU')))

# hide gpu for cpu usage
#tf.config.set_visible_devices([], 'GPU')
#tf.keras.mixed_precision.set_global_policy('mixed_float16')

STEP_IDX = 0
ACCURACY_LOG = f'accuracy_log{STEP_IDX}.txt'

def keep_strongest(weights):
	factor = 0.25
	#for row in weights:
	#	max = np.max(np.abs(row))
	#	row[np.abs(row) < max * factor] = 0
	for column in weights.T:
		max = np.max(np.abs(column))
		column[np.abs(column) < max * factor] = 0

def compile_model(model):
	opt = tf.keras.optimizers.Nadam(learning_rate=0.003)
	model.compile(optimizer=opt, loss='binary_crossentropy', metrics=[tf.keras.metrics.BinaryAccuracy()])

class SnormConstraint(tf.keras.constraints.Constraint):
# constrain each value to be between -1 and 1

	def __init__(self):
		pass

	def __call__(self, w):
		# find the largest value in tensor
		maxval = tf.reduce_max(tf.abs(w))
		# divide by max
		return w / maxval
		#return tf.clip_by_value(w, -1.0, 1.0)

	def get_config(self):
		return {}

def build_net_relu(n_hidden = LAYERS, n_neurons = NEURONS):
	kernel_constraint = None#SnormConstraint()
	bias_constraint = None

	model = keras.models.Sequential()
	model.add(keras.Input(shape=(5,)))
	for layer in range(n_hidden):
		model.add(keras.layers.Dense(n_neurons, activation="relu", kernel_initializer="he_normal", kernel_constraint=kernel_constraint, bias_constraint=bias_constraint))
		#model.add(keras.layers.Dense(n_neurons, activation=LeakyReLU(alpha=0.1), kernel_initializer="he_normal"))
	# final layer for binary classification
	model.add(keras.layers.Dense(1, activation="sigmoid", kernel_constraint=kernel_constraint, bias_constraint=bias_constraint))
	opt = tf.keras.optimizers.Nadam(learning_rate=0.003)
	model.compile(optimizer=opt, loss='mse', metrics=[tf.keras.metrics.BinaryAccuracy()])
	return model

def print_stats(clf, rasterf, requiredf):
	# predict test data
	y_pred = clf.predict(rasterf).reshape(-1)
	y_pred = np.where(y_pred > 0.5, 1, 0)
	# print test set stats
	acc = np.count_nonzero(y_pred == requiredf) / len(y_pred)
	print("accuracy training: ", acc)
	print("confusion matrix training:")
	cm = confusion_matrix(requiredf, y_pred)
	print(cm)
	f1 = f1_score(requiredf, y_pred)
	print("F1 score: ", f1)
	print("Recall: ", cm[1][1] / (cm[1][1] + cm[1][0]))

def inspectSample(stepIndex, batch_size):
	tf.keras.utils.set_random_seed(1) # use same random seed for training
	filename = f"{ML_NAME}{stepIndex}.pkl"

	print(f"--------- SAMPLE {stepIndex} -------------------------------------------------")



	rasterf = np.load(f'train/raster_train{stepIndex}_.npy')
	requiredf = np.load(f'train/required_train{stepIndex}_.npy').reshape(-1)
	weightf = np.load(f'train/weight_train{stepIndex}_.npy').reshape(-1)
	raster_validationf = np.load(f'eval/raster_eval{stepIndex}_0.npy')
	required_validationf = np.load(f'eval/required_eval{stepIndex}_0.npy').reshape(-1)

	numClass1 = np.count_nonzero(requiredf)
	numClass0 = len(requiredf) - numClass1
	class_weight = {0: max(numClass1/numClass0, 1.0), 1: max(numClass0/numClass1, 1.0)}
	
	print("length of data: ", len(rasterf))
	print("class weight: ", class_weight)

	clf = None
	try:
		if CLEAR_FILE and os.path.exists(filename):
			os.remove(filename)

		clf = pickle.load(open(filename, 'rb'))
	except:
		print('no saved model found -> training new model')
		net = build_net_relu()
		
		clf = Pipeline(steps=[
			('filter', FilterInputsTransformer()),
			('clamper', ClampTransformer(16)),
			('net', net)
		])

		clf = clf.fit(rasterf, requiredf, 
			net__epochs=1000, # 1000 
			#net__validation_data=(raster_validationf, required_validationf), 
			net__verbose=1, 
			net__batch_size=batch_size, #8192
			net__class_weight=class_weight,
			net__callbacks=[EarlyStopping(monitor='binary_accuracy', patience=50, min_delta=0.001, start_from_epoch=10)]
		)
		# also save in file
		pickle.dump(clf, open(filename, "wb"))

	# predict test data
	# print weights
	weights = clf.named_steps['net'].get_weights()
	for i in range(len(weights)):
		print("weights", i, ":", weights[i])

	# take network and refine weights
	'''clf2 = pickle.load(open(filename, 'rb')) # load from file again
	net2 = clf2.named_steps['net']

	REFINE_EPOCHS = 6

	for _ in range(REFINE_EPOCHS): # layer 0
		clf2.fit(rasterf, requiredf, net__epochs=1, net__verbose=1, net__batch_size=batch_size, net__class_weight=class_weight)
		# modify layer weights
		weights0 = net2.get_layer(index=0).get_weights()
		keep_strongest(weights0[0])
		net2.get_layer(index=0).set_weights(weights0)

	# fix weights of layer 0
	net2.get_layer(index=0).trainable = False
	compile_model(net2)

	for _ in range(REFINE_EPOCHS):
		clf2.fit(rasterf, requiredf, net__epochs=1, net__verbose=1, net__batch_size=batch_size, net__class_weight=class_weight)
		# modify layer weights
		weights0 = net2.get_layer(index=1).get_weights()
		keep_strongest(weights0[0])
		net2.get_layer(index=1).set_weights(weights0)

	# fix weights of layer 1
	net2.get_layer(index=1).trainable = False
	compile_model(net2)
	# final train (on last layer)
	clf2.fit(rasterf, requiredf, net__epochs=100, net__verbose=1, net__batch_size=batch_size, net__class_weight=class_weight,
	net__callbacks=[EarlyStopping(monitor='binary_accuracy', patience=5, min_delta=0.001, start_from_epoch=REFINE_EPOCHS)])
	
	weights0 = net2.get_layer(index=0).get_weights()
	weights1 = net2.get_layer(index=1).get_weights()
	weights2 = net2.get_layer(index=2).get_weights()
	print("kernel0: ", np.round(weights0[0], 2))
	print("kernel1: ", np.round(weights1[0], 2))
	print("kernel2: ", np.round(weights2[0], 2))
	print("bias0: ", np.round(weights0[1], 2))
	print("bias1: ", np.round(weights1[1], 2))
	print("bias2: ", np.round(weights2[1], 2))'''
	
	print("------- original test -------")
	print_stats(clf, rasterf, requiredf)
	#print("------- refined test -------")
	#print_stats(clf2, rasterf, requiredf)

	print("------- original valid -------")
	print_stats(clf, raster_validationf, required_validationf)

	'''[[ 703510   68179] training ref 0.8696727055933129
 	   [ 228246 1274531]]'''
	'''[[ 50271   3182]  validation ref 0.8901166274192874
 	    [ 20476 141372]]'''

	#print("------- refined valid -------")
	#print_stats(clf2, raster_validationf, required_validationf)

	# print rounded weights
	

	print("----------------------------------------------------------------")
	#tree.plot_tree(clf)
	#plt.show()


#inspectSample(3)

#for i in range(NUM_STEPS):
#for rng_seed in range(4):
inspectSample(STEP_IDX, 1024)

#with open(ACCURACY_LOG, 'a') as f:
#	f.write('----------------------------------------------------------------\n')
