# idea: take all 32 samples but train only 4 estimators (one for each step)

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
import pickle

NUM_DIRECTIONS = 8
NUM_STEPS = 4
NUM_SAMPLES = NUM_DIRECTIONS * NUM_STEPS
CLEAR_FILE = False # clears cached files

# set current directory as working directory
os.chdir(os.path.dirname(os.path.abspath(__file__)))

raster = np.load('raster_train.npy')
ray = np.load('ray_train.npy')
sphereStart = np.load('sphereStart_train.npy')
print("length of input data: ", len(raster))

raster_validation = np.load('raster_validation.npy')
ray_validation = np.load('ray_validation.npy')
sphereStart_validation = np.load('sphereStart_validation.npy')
#pixelXY = np.load('pixelXY.npy')



def inspectSample(stepIndex):

	filename = f"rnd_forest_{stepIndex}.pkl"

	print(f"--------- SAMPLE {stepIndex} -------------------------------------------------")

	rasterf = None
	different = None
	raster_validationf = None
	different_validation = None

	for dir in range(NUM_DIRECTIONS):
		print(f"direction {dir+1}/{NUM_DIRECTIONS}")
		i = dir * NUM_STEPS + stepIndex
		# filter data
		filter = [raster[j, i] > sphereStart[j, i] for j in range(len(raster))]
		rayf0 = ray[filter]
		rasterf0 = raster[filter]

		# filter validation set
		filter = [raster_validation[j, i] > sphereStart_validation[j, i] for j in range(len(raster_validation))]
		ray_validationf0 = ray_validation[filter]
		raster_validationf0 = raster_validation[filter]

		different0 = [rayf0[j, i] < rasterf0[j, i] for j in range(len(rayf0))]
		
		different_validation0 = [ray_validationf0[j, i] < raster_validationf0[j, i] for j in range(len(ray_validationf0))]

		# shift samples
		if dir == 0:
			# init vectors
			rasterf = rasterf0
			different = different0
			raster_validationf = raster_validationf0
			different_validation = different_validation0
		else:
			# rotate vectors
			rasterf0 = np.roll(rasterf0, -4 * dir, axis=1)
			raster_validationf0 = np.roll(raster_validationf0, -4 * dir, axis=1)
			# concatenate vectors
			rasterf = np.concatenate((rasterf, rasterf0), axis=0)
			different = np.concatenate((different, different0), axis=0)
			raster_validationf = np.concatenate((raster_validationf, raster_validationf0), axis=0)
			different_validation = np.concatenate((different_validation, different_validation0), axis=0)


	print("length of filtered data: ", len(rasterf))

	# split rasterf and different into training and test data
	x_train, x_test, y_train, y_test = train_test_split(rasterf, different, test_size=0.2, random_state=0)

	clf = None
	try:
		if CLEAR_FILE and os.path.exists(filename):
			os.remove(filename)

		clf = pickle.load(open(filename, 'rb'))
	except:
		print('no saved model found -> training new model')
		# train decision tree classifier
		#clf = tree.DecisionTreeClassifier(random_state=0) # TODO random forest?
		clf = RandomForestClassifier(
			random_state=0, 
			n_estimators=100, 
			#max_features=None, # all features
			bootstrap=True,
			max_samples=min(4000000, len(x_train)), # 4M samples
			#class_weight={True: 1, False: 100}, # increase false weight to punish false negatives (non-raytraced samples that need to be ray traced) => higher image quality but less performance
			#max_depth=10,
			n_jobs=-1)
		clf = clf.fit(x_train, y_train)
		# also save in file
		pickle.dump(clf, open(filename, "wb"))

	# predict test data
	y_pred = clf.predict(x_test)
	# print test set stats
	print("accuracy test: ", np.count_nonzero(y_pred == y_test) / len(y_test))
	print("confusion matrix test:")
	print(confusion_matrix(y_test, y_pred))

	# print validation set stats
	y_pred = clf.predict(raster_validationf)
	print("accuracy validation: ", np.count_nonzero(y_pred == different_validation) / len(different_validation))
	print("confusion matrix validation:")
	print(confusion_matrix(different_validation, y_pred))

	#tree_importance = clf.tree_.compute_feature_importances(normalize=True)
	#tree_importance = clf.tree_.compute_feature_importances(normalize=True)
	tree_importance = clf.feature_importances_
	#tree_importance = sum([tree.feature_importances_ for tree in clf.estimators_])
	# write feature importances to numpy 2d array for visualization
	importance = np.zeros((9, 9))
	importance[3, 4] = tree_importance[0]
	importance[2, 4] = tree_importance[1]
	importance[1, 4] = tree_importance[2]
	importance[0, 4] = tree_importance[3]
	importance[3, 5] = tree_importance[4]
	importance[2, 6] = tree_importance[5]
	importance[1, 7] = tree_importance[6]
	importance[0, 8] = tree_importance[7]
	importance[4, 5] = tree_importance[8]
	importance[4, 6] = tree_importance[9]
	importance[4, 7] = tree_importance[10]
	importance[4, 8] = tree_importance[11]
	importance[5, 5] = tree_importance[12]
	importance[6, 6] = tree_importance[13]
	importance[7, 7] = tree_importance[14]
	importance[8, 8] = tree_importance[15]
	importance[5, 4] = tree_importance[16]
	importance[6, 4] = tree_importance[17]
	importance[7, 4] = tree_importance[18]
	importance[8, 4] = tree_importance[19]
	importance[5, 3] = tree_importance[20]
	importance[6, 2] = tree_importance[21]
	importance[7, 1] = tree_importance[22]
	importance[8, 0] = tree_importance[23]
	importance[4, 3] = tree_importance[24]
	importance[4, 2] = tree_importance[25]
	importance[4, 1] = tree_importance[26]
	importance[4, 0] = tree_importance[27]
	importance[3, 3] = tree_importance[28]
	importance[2, 2] = tree_importance[29]
	importance[1, 1] = tree_importance[30]
	importance[0, 0] = tree_importance[31]
	# save numpy array in file
	np.save("importance" + str(stepIndex) + ".npy", importance)
	print("----------------------------------------------------------------")
	#tree.plot_tree(clf)
	#plt.show()


#inspectSample(3)

for i in range(NUM_STEPS):
	inspectSample(i)
