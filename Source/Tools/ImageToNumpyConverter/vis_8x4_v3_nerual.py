import numpy as np
import os
import matplotlib.pyplot as plt
from sklearn.model_selection import cross_val_score
from sklearn.linear_model import LogisticRegression
from sklearn.metrics import confusion_matrix
from sklearn.preprocessing import StandardScaler
from sklearn.pipeline import Pipeline
from sklearn import tree
from sklearn.model_selection import train_test_split
from sklearn.ensemble import RandomForestClassifier

import tensorflow as tf
from tensorflow import keras

print("TensorFlow version: ", tf.__version__)
print("Keras version: ", keras.__version__)

NUM_DIRECTIONS = 8
NUM_STEPS = 4
NUM_SAMPLES = NUM_DIRECTIONS * NUM_STEPS

# set current directory as working directory
os.chdir(os.path.dirname(os.path.abspath(__file__)))

# TODO load from file?

raster = np.load('raster_train.npy')
ray = np.load('ray_train.npy')
sphereStart = np.load('sphereStart_train.npy')
print("length of input data: ", len(raster))

raster_validation = np.load('raster_validation.npy')
ray_validation = np.load('ray_validation.npy')
sphereStart_validation = np.load('sphereStart_validation.npy')
#pixelXY = np.load('pixelXY.npy')

def boolToFloat(b):
	return 1.0 if b else 0.0

def floatToBool(f):
	return True if f > 0.5 else False

def inspectSample(i):

	print(f"--------- SAMPLE {i} -------------------------------------------------")

	# filter data
	filter = [raster[j, i] > sphereStart[j, i] for j in range(len(raster))]
	rayf = ray[filter]
	rasterf = raster[filter]
	print("length of filtered data: ", len(rasterf))

	# filter validation set
	filter = [raster_validation[j, i] > sphereStart_validation[j, i] for j in range(len(raster_validation))]
	ray_validationf = ray_validation[filter]
	raster_validationf = raster_validation[filter]

	different = np.array([(rayf[j, i] < rasterf[j, i]) for j in range(len(rayf))])
	print(f"different: {np.count_nonzero(different)} / {len(different)} ({round(np.count_nonzero(different) / len(different) * 100)}%)")

	different_validation = np.array([(ray_validationf[j, i] < raster_validationf[j, i]) for j in range(len(ray_validationf))])

	# split rasterf and different into training and test data
	x_train, x_test, y_train, y_test = train_test_split(rasterf, different, test_size=0.2, random_state=0)

	# create tensorflow dataset
	train_data = tf.data.Dataset.from_tensor_slices((x_train, [boolToFloat(f) for f in y_train]))
	test_data = tf.data.Dataset.from_tensor_slices((x_test, [boolToFloat(f) for f in y_test]))
	validation_data = tf.data.Dataset.from_tensor_slices((raster_validationf, [boolToFloat(f) for f in different_validation]))

	BATCH_SIZE = 32
	train_data = train_data.batch(BATCH_SIZE)
	test_data = test_data.batch(BATCH_SIZE)
	validation_data = validation_data.batch(BATCH_SIZE)

	# build neural network
	model = keras.Sequential()
	model.add(keras.Input(shape=(NUM_SAMPLES,)))
	model.add(keras.layers.Dense(64, activation='sigmoid')) 
	model.add(keras.layers.Dense(64, activation='sigmoid')) 
	model.add(keras.layers.Dense(64, activation='sigmoid')) 
	model.add(keras.layers.Dense(64, activation='sigmoid')) 
	model.add(keras.layers.Dense(1, activation='sigmoid')) # single output
	#model.compile(optimizer='adam', loss='binary_crossentropy', metrics=['accuracy'])
	#model.compile(optimizer='adam', loss='binary_crossentropy', metrics=[tf.keras.metrics.TruePositives()])
	model.compile(optimizer='adam', loss='binary_crossentropy', metrics=[tf.keras.metrics.PrecisionAtRecall(0.75)]) # 75% of ray tracing samples are found (at most 25% image error)

	# train neural network
	model.fit(train_data, epochs=10, verbose=1, validation_data=validation_data) # , class_weight={0: 1.0, 1: 10.0}

	# predict test data
	y_pred = np.array([floatToBool(f) for f in model.predict(x_test)])
	# print test set stats
	
	print("accuracy test: ", np.count_nonzero(y_pred == y_test) / len(y_test))
	print("confusion matrix test:")
	print(confusion_matrix(y_test, y_pred))

	# print validation set stats
	y_pred = np.array([floatToBool(f) for f in model.predict(raster_validationf)])
	print("accuracy validation: ", np.count_nonzero(y_pred == different_validation) / len(different_validation))
	print("confusion matrix validation:")
	print(confusion_matrix(different_validation, y_pred))

	#tree_importance = clf.tree_.compute_feature_importances(normalize=True)
	#tree_importance = clf.tree_.compute_feature_importances(normalize=True)
	#tree_importance = clf.feature_importances_
	#tree_importance = sum([tree.feature_importances_ for tree in clf.estimators_])
	# write feature importances to numpy 2d array for visualization
	#importance = np.zeros((9, 9))
	#importance[3, 4] = tree_importance[0]
	#importance[2, 4] = tree_importance[1]
	#importance[1, 4] = tree_importance[2]
	#importance[0, 4] = tree_importance[3]
	#importance[3, 5] = tree_importance[4]
	#importance[2, 6] = tree_importance[5]
	#importance[1, 7] = tree_importance[6]
	#importance[0, 8] = tree_importance[7]
	#importance[4, 5] = tree_importance[8]
	#importance[4, 6] = tree_importance[9]
	#importance[4, 7] = tree_importance[10]
	#importance[4, 8] = tree_importance[11]
	#importance[5, 5] = tree_importance[12]
	#importance[6, 6] = tree_importance[13]
	#importance[7, 7] = tree_importance[14]
	#importance[8, 8] = tree_importance[15]
	#importance[5, 4] = tree_importance[16]
	#importance[6, 4] = tree_importance[17]
	#importance[7, 4] = tree_importance[18]
	#importance[8, 4] = tree_importance[19]
	#importance[5, 3] = tree_importance[20]
	#importance[6, 2] = tree_importance[21]
	#importance[7, 1] = tree_importance[22]
	#importance[8, 0] = tree_importance[23]
	#importance[4, 3] = tree_importance[24]
	#importance[4, 2] = tree_importance[25]
	#importance[4, 1] = tree_importance[26]
	#importance[4, 0] = tree_importance[27]
	#importance[3, 3] = tree_importance[28]
	#importance[2, 2] = tree_importance[29]
	#importance[1, 1] = tree_importance[30]
	#importance[0, 0] = tree_importance[31]
	# save numpy array in file
	#np.save("importance" + str(i) + ".npy", importance)
	print("----------------------------------------------------------------")
	#tree.plot_tree(clf)
	#plt.show()


inspectSample(3)

#for i in range(NUM_SAMPLES):
#	inspectSample(i)