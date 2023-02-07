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
NEURONS = 4
#ML_NAME = f"net_{LAYERS}_{NEURONS}_"
ML_NAME = f"net_relu"

# set current directory as working directory
os.chdir(os.path.dirname(os.path.abspath(__file__)))

print("TensorFlow version: ", tf.__version__)
print("Num GPUs Available: ", len(tf.config.list_physical_devices('GPU')))

# hide gpu for cpu usage
#tf.config.set_visible_devices([], 'GPU')
#tf.keras.mixed_precision.set_global_policy('mixed_float16')

STEP_IDX = 3
ACCURACY_LOG = f'accuracy_log{STEP_IDX}.txt'

def build_net(n_hidden, n_neurons):
	model = keras.models.Sequential()
	model.add(keras.Input(shape=(NUM_SAMPLES,)))
	for layer in range(n_hidden):
		model.add(keras.layers.Dense(n_neurons, activation="selu", kernel_initializer="lecun_normal"))
	# final layer for binary classification
	model.add(keras.layers.Dense(1, activation="sigmoid"))
	opt = tf.keras.optimizers.Nadam(learning_rate=0.003)
	model.compile(optimizer=opt, loss='binary_crossentropy', metrics=[tf.keras.metrics.BinaryAccuracy()])
	return model

def build_net_relu(n_hidden = LAYERS, n_neurons = NEURONS):
	model = keras.models.Sequential()
	model.add(keras.Input(shape=(5,)))
	for layer in range(n_hidden):
		model.add(keras.layers.Dense(n_neurons, activation="relu", kernel_initializer="he_normal"))
		#model.add(keras.layers.Dense(n_neurons, activation=LeakyReLU(alpha=0.1), kernel_initializer="he_normal"))
	# final layer for binary classification
	model.add(keras.layers.Dense(1, activation="sigmoid"))
	opt = tf.keras.optimizers.Nadam(learning_rate=0.003)
	model.compile(optimizer=opt, loss='binary_crossentropy', metrics=[tf.keras.metrics.BinaryAccuracy()])
	return model

def inspectSample(stepIndex, batch_size):
	tf.keras.utils.set_random_seed(0) # use same random seed for training
	filename = f"{ML_NAME}{stepIndex}.pkl"

	print(f"--------- SAMPLE {stepIndex} -------------------------------------------------")



	rasterf = np.load(f'train/raster_train{stepIndex}_.npy')
	requiredf = np.load(f'train/required_train{stepIndex}_.npy').reshape(-1)
	weightf = np.load(f'train/weight_train{stepIndex}_.npy').reshape(-1)
	raster_validationf = np.load(f'eval/raster_eval{stepIndex}_0.npy')
	required_validationf = np.load(f'eval/required_eval{stepIndex}_0.npy').reshape(-1)

	print("length of data: ", len(rasterf))

	clf = None
	try:
		if CLEAR_FILE and os.path.exists(filename):
			os.remove(filename)

		clf = pickle.load(open(filename, 'rb'))
	except:
		print('no saved model found -> training new model')
		# train decision tree classifier
		#clf = tree.DecisionTreeClassifier(random_state=0) # TODO random forest?
		#poly = PolynomialFeatures(2, interaction_only=True, include_bias=True)
		scaler = StandardScaler()
		#scaler = RobustScaler()
		#estimator = RandomForestClassifier(
		#	random_state=0, 
		#	n_estimators=100, 
		#	#max_features=None, # all features
		#	bootstrap=True,
		#	max_samples=min(10000000, len(rasterf)), # 4M samples
		#	#class_weight={True: 1, False: 100}, # increase false weight to punish false negatives (non-raytraced samples that need to be ray traced) => higher image quality but less performance
		#	#max_depth=10,
		#	n_jobs=-1)
		params = {
			#"n_hidden": [6],
			"n_neurons": [4, 8, 12, 16],
			#"learning_rate": [0.001, 0.005, 0.01]
		}

		#rnd_search = RandomizedSearchCV(kreas_reg, params, n_iter=10, cv=4, verbose=3, n_jobs=-1)
		#grid_search = GridSearchCV(kreas_reg, params, cv=4, verbose=3)
		#net = None
		#net = keras.wrappers.scikit_learn.KerasClassifier(build_net(LAYERS, NEURONS, 0.005))
		#net = build_net()
		net = build_net_relu()
		#net.summary()
		
		numClass1 = np.count_nonzero(requiredf)
		numClass0 = len(requiredf) - numClass1
		class_weight = {0: max(numClass1/numClass0, 1.0), 1: max(numClass0/numClass1, 1.0)}
		print("class weight: ", class_weight)
		#class_weight = {0: 1.0, 1: 10.0}

		#clf = Pipeline(steps=[('scaler', scaler), ('net', rnd_search)])
		clf = Pipeline(steps=[
			('filter', FilterInputsTransformer()),
			('clamper', ClampTransformer(16)),
			#('scaler', scaler), 
			('net', net)
		])

		clf = clf.fit(rasterf, requiredf, 
			net__epochs=1000, 
			#net__validation_data=(raster_validationf, required_validationf), 
			net__verbose=1, 
			net__batch_size=batch_size, #8192
			net__class_weight=class_weight,
			net__callbacks=[EarlyStopping(monitor='binary_accuracy', patience=50, min_delta=0.001, start_from_epoch=10)]
		)
		# also save in file
		pickle.dump(clf, open(filename, "wb"))

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

	#with open(ACCURACY_LOG, 'a') as f:
	#	f.write(f'TRAIN step {stepIndex}  batch {batch_size} acc:\t{acc} \tf1: {f1} \n[[{cm[0][0]} {cm[0][1]}]\n [{cm[1][0]} {cm[1][1]}]]\n')

	# print validation set stats
	#y_pred = clf.predict(raster_validationf).reshape(-1)
	y_pred = clf.predict(raster_validationf).reshape(-1)
	y_pred = np.where(y_pred > 0.5, 1, 0)

	acc = np.count_nonzero(y_pred == required_validationf) / len(y_pred)
	print("accuracy validation: ", acc)
	print("confusion matrix validation:")
	cm = confusion_matrix(required_validationf, y_pred)
	# f1 score
	print(cm)
	f1 = f1_score(required_validationf, y_pred)
	print("F1 score: ", f1)
	print("Recall: ", cm[1][1] / (cm[1][1] + cm[1][0]))

	# append accuracy to txt file
	#with open(ACCURACY_LOG, 'a') as f:
	#	f.write(f'VALID step {stepIndex} batch {batch_size} acc:\t{acc} \tf1: {f1} \n[[{cm[0][0]} {cm[0][1]}]\n [{cm[1][0]} {cm[1][1]}]]\n')
	#	f.write('\n')

	#grid = clf.named_steps['net']
	#print(grid.cv_results_['params'][grid.best_index_])
	#print(grid.best_params_)
	#print(grid.best_score_)

	print("----------------------------------------------------------------")
	#tree.plot_tree(clf)
	#plt.show()
	return acc


#inspectSample(3)

#for i in range(NUM_STEPS):
#for rng_seed in range(4):
acc = inspectSample(STEP_IDX, 1024)

#with open(ACCURACY_LOG, 'a') as f:
#	f.write('----------------------------------------------------------------\n')
