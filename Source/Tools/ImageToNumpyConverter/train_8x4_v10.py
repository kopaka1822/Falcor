# idea: train logistic regression on all 32 samples

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
from sklearn.svm import LinearSVC
from sklearn.ensemble import BaggingClassifier
from sklearn.ensemble import AdaBoostClassifier

import pickle

NUM_DIRECTIONS = 8
NUM_STEPS = 4
NUM_SAMPLES = NUM_DIRECTIONS * NUM_STEPS
CLEAR_FILE = False # clears cached files
ML_NAME = "rnd_forest"

# set current directory as working directory
os.chdir(os.path.dirname(os.path.abspath(__file__)))

def inspectSample(stepIndex):

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
		#clf = tree.DecisionTreeClassifier(random_state=0)
		clf = RandomForestClassifier(
			random_state=0, 
			n_estimators=96, 
			#max_features=None, # all features
			bootstrap=True,
			max_samples=min(300000, len(rasterf)), # 4M samples
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

	#grid = clf.named_steps['estimator']
	#print(grid.cv_results_['params'][grid.best_index_])

	print("----------------------------------------------------------------")
	#tree.plot_tree(clf)
	#plt.show()


#inspectSample(0)

for i in range(NUM_STEPS):
	inspectSample(i)
