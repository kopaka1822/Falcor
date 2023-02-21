# idea: random forest on vao samples

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
from sklearn.model_selection import GridSearchCV
from sklearn.preprocessing import PolynomialFeatures
from sklearn.preprocessing import RobustScaler
from sklearn.svm import LinearSVC
from sklearn.ensemble import BaggingClassifier
from sklearn.ensemble import AdaBoostClassifier
from sklearn.base import BaseEstimator, TransformerMixin
from sklearn.decomposition import PCA
from sklearn.svm import SVC

import pickle

NUM_SAMPLES = 8
CLEAR_FILE = True # clears cached files
ML_NAME = "forest"

# set current directory as working directory
os.chdir(os.path.dirname(os.path.abspath(__file__)))

# load train and evaluation data
raster = np.load(f'train/raster_train_.npy')
required = np.load(f'train/required_train_.npy')
asked = np.load(f'train/asked_train_.npy')

raster_validation = np.load(f'eval/raster_eval_0.npy')
required_validation = np.load(f'eval/required_eval_0.npy')
asked_validation = np.load(f'eval/asked_eval_0.npy')

# process data (clip)
raster = np.clip(raster, -16, 16)
raster_validation = np.clip(raster_validation, -16, 16)

def inspectSample(stepIndex):

	filename = f"{ML_NAME}{stepIndex}.pkl"

	print(f"--------- SAMPLE {stepIndex} -------------------------------------------------")

	# filter results (only take the stepIndex column)
	requiredf = required[:, stepIndex]
	askedf = asked[:, stepIndex]
	required_validationf = required_validation[:, stepIndex]
	asked_validationf = asked_validation[:, stepIndex]

	# only take samples where asked is 1
	rasterf = raster[askedf == 1]
	requiredf = requiredf[askedf == 1]
	raster_validationf = raster_validation[asked_validationf == 1]
	required_validationf = required_validationf[asked_validationf == 1]
	

	print("length of data: ", len(rasterf))

	clf = None
	try:
		if CLEAR_FILE and os.path.exists(filename):
			os.remove(filename)

		clf = pickle.load(open(filename, 'rb'))
	except:
		print('no saved model found -> training new model')
		# train random forest
		clf = RandomForestClassifier(
			n_estimators=96, 
			random_state=0,
			bootstrap=True,
			max_samples=min(1000000, len(rasterf)),
			n_jobs=-1
		)

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
	cm = confusion_matrix(required_validationf, y_pred)
	print(cm)
	print("Recall: ", cm[1][1] / (cm[1][1] + cm[1][0]))

	#grid = clf.named_steps['estimator']
	#print(grid.cv_results_['params'][grid.best_index_])

	print("----------------------------------------------------------------")
	#tree.plot_tree(clf)
	#plt.show()

	tree_importance = clf.feature_importances_
	importance = np.zeros(len(tree_importance))
	for j in range(len(tree_importance)):
		importance[j] = tree_importance[j]

	# save numpy array in file
	np.save("importance" + str(stepIndex) + ".npy", importance)


#inspectSample(3)

for i in range(NUM_SAMPLES):
	inspectSample(i)
