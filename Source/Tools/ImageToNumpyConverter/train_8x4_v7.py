# idea: take all only 8 samples and train 4 classifiers

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

indices = np.array([3, 2, 1, 0, 16, 17, 18, 19])

raster = [None] * NUM_STEPS
required = [None] * NUM_STEPS
weight = [None] * NUM_STEPS
for i in range(NUM_STEPS):
	raster[i] = np.load(f'train/raster_train{i}_.npy')
	required[i] = np.load(f'train/required_train{i}_.npy').reshape(-1)
	weight[i] = np.load(f'train/weight_train{i}_.npy').reshape(-1)
	print(f"length of data {i}: ", len(raster[i]))

	# filter with indices
	raster[i] = np.take(raster[i], indices, axis=1)

raster_validation = [None] * NUM_STEPS
required_validation = [None] * NUM_STEPS
for i in range(NUM_STEPS):
	raster_validation[i] = np.load(f'eval/raster_eval{i}_0.npy')
	required_validation[i] = np.load(f'eval/required_eval{i}_0.npy').reshape(-1)
	# filter with indices
	raster_validation[i] = np.take(raster_validation[i], indices, axis=1)

def inspectSample(stepIndex):

	filename = f"rnd_forest_{stepIndex}.pkl"

	print(f"--------- SAMPLE {stepIndex} -------------------------------------------------")



	rasterf = raster[stepIndex]
	requiredf = required[stepIndex]
	weightf = weight[stepIndex]
	raster_validationf = raster_validation[stepIndex]
	required_validationf = required_validation[stepIndex]

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
		clf = RandomForestClassifier(
			random_state=0, 
			n_estimators=100, 
			#max_features=None, # all features
			bootstrap=True,
			max_samples=min(10000000, len(rasterf)), # 4M samples
			#class_weight={True: 1, False: 100}, # increase false weight to punish false negatives (non-raytraced samples that need to be ray traced) => higher image quality but less performance
			#max_depth=10,
			n_jobs=-1)
		clf = clf.fit(rasterf, requiredf)
		# also save in file
		pickle.dump(clf, open(filename, "wb"))

	# predict test data
	y_pred = clf.predict(rasterf)
	# print test set stats
	print("accuracy training: ", np.count_nonzero(y_pred == requiredf) / len(y_pred))
	print("confusion matrix training:")
	print(confusion_matrix(requiredf, y_pred))

	# print validation set stats
	y_pred = clf.predict(raster_validationf)
	print("accuracy validation: ", np.count_nonzero(y_pred == required_validationf) / len(y_pred))
	print("confusion matrix validation:")
	print(confusion_matrix(required_validationf, y_pred))

	#tree_importance = clf.tree_.compute_feature_importances(normalize=True)
	#tree_importance = clf.tree_.compute_feature_importances(normalize=True)
	tree_importance = clf.feature_importances_
	#tree_importance = sum([tree.feature_importances_ for tree in clf.estimators_])
	# write feature importances to numpy 2d array for visualization
	importance = np.zeros(len(tree_importance))
	for j in range(len(tree_importance)):
		importance[j] = tree_importance[j]

	# save numpy array in file
	np.save("importance" + str(stepIndex) + ".npy", importance)
	print("----------------------------------------------------------------")
	#tree.plot_tree(clf)
	#plt.show()


#inspectSample(3)

for i in range(NUM_STEPS):
	inspectSample(i)
