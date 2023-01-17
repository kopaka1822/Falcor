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

def getSampleMask(i):
	# divide i by 4 with integer division
	i = i // 4
	if i == 0 or i == 4:
		return np.array([True, True, True, True, False, False, False, False, False, False, False, False, False, False, False, False, True, True, True, True, False, False, False, False, False, False, False, False, False, False, False, False])
	elif i == 1 or i == 5:
		return np.array([False, False, False, False, True, True, True, True, False, False, False, False, False, False, False, False, False, False, False, False, True, True, True, True, False, False, False, False, False, False, False, False])
	elif i == 2 or i == 6:
		return np.array([False, False, False, False, False, False, False, False, True, True, True, True, False, False, False, False, False, False, False, False, False, False, False, False, True, True, True, True, False, False, False, False])
	elif i == 3 or i == 7:
		return np.array([False, False, False, False, False, False, False, False, False, False, False, False, True, True, True, True, False, False, False, False, False, False, False, False, False, False, False, False, True, True, True, True])
	return None
	

def inspectSample(i):

	filename = f"rnd_forest_{i}.pkl"

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

	different = [rayf[j, i] < rasterf[j, i] for j in range(len(rayf))]
	print(f"different: {different.count(True)} / {len(different)} ({round(different.count(True) / len(different) * 100)}%)")

	different_validation = [ray_validationf[j, i] < raster_validationf[j, i] for j in range(len(ray_validationf))]

	# filter raster further to only include the sample mask
	# convert 32 input samples to 8 input samples (straight line only)
	sample_mask = getSampleMask(i)
	rasterf = rasterf[:, sample_mask]
	raster_validationf = raster_validationf[:, sample_mask]

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
			max_samples=min(3000000, len(x_train)), # 3m samples
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
	cm = confusion_matrix(y_test, y_pred)
	print(cm)
	print("Recall: ", cm[1, 1] / (cm[1, 1] + cm[1, 0]))

	# print validation set stats
	y_pred = clf.predict(raster_validationf)
	print("accuracy validation: ", np.count_nonzero(y_pred == different_validation) / len(different_validation))
	print("confusion matrix validation:")
	cm = confusion_matrix(different_validation, y_pred)
	print(cm)
	print("Recall: ", cm[1, 1] / (cm[1, 1] + cm[1, 0]))

	#tree_importance = clf.tree_.compute_feature_importances(normalize=True)
	#tree_importance = clf.tree_.compute_feature_importances(normalize=True)
	tree_importance = clf.feature_importances_
	#tree_importance = sum([tree.feature_importances_ for tree in clf.estimators_])
	# write feature importances to numpy 2d array for visualization
	importance = np.zeros(len(tree_importance))
	for i in range(len(tree_importance)):
		importance[i] = tree_importance[i]
	# save numpy array in file
	np.save("importance" + str(i) + ".npy", importance)
	print("----------------------------------------------------------------")
	#tree.plot_tree(clf)
	#plt.show()


#inspectSample(7)

for i in range(NUM_SAMPLES):
	inspectSample(i)
