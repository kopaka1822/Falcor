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
from sklearn.preprocessing import RobustScaler
from sklearn.svm import LinearSVC
from sklearn.ensemble import BaggingClassifier
from sklearn.ensemble import AdaBoostClassifier
from sklearn.base import BaseEstimator, TransformerMixin
from sklearn.decomposition import PCA
from sklearn.svm import SVC

import pickle

NUM_DIRECTIONS = 8
NUM_STEPS = 4
NUM_SAMPLES = NUM_DIRECTIONS * NUM_STEPS
CLEAR_FILE = True # clears cached files
ML_NAME = "svc_bag"

# set current directory as working directory
os.chdir(os.path.dirname(os.path.abspath(__file__)))

class ClampTransformer(BaseEstimator, TransformerMixin):
	def __init__(self):
		self.value = 10.0

	def fit(self, X, y=None):
		return self

	def transform(self, X, y=None):
		X = np.clip(X, -self.value, self.value)
		return X

class FeatureFilter(BaseEstimator, TransformerMixin):
	def __init__(self):
		None
	
	def fit(self, X, y=None):
		return self

	def transform(self, X, y=None):
		X = X[:, np.array([True, True, True, True, False, False, False, False, False, False, False, False, False, False, False, False, True, True, True, True, False, False, False, False, False, False, False, False, False, False, False, False])]
		return X

class RelativeHeights(BaseEstimator, TransformerMixin):
	def __init__(self):
		None
	
	def fit(self, X, y=None):
		return self

	def transform(self, X, y=None):
		for i in range(0, 32, 4):
			X[:,i+1] = X[:,i+1] - X[:,i]
			X[:,i+2] = X[:,i+2] - X[:,i]
			X[:,i+3] = X[:,i+3] - X[:,i]
		return X

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
		#scaler = StandardScaler(with_mean=False)
		robustscaler = RobustScaler(with_centering=False)
		scaler = ClampTransformer()
		#stimator = LinearSVC(
		#	random_state=0,
		#	C=100,
		#	dual=False,
		#
		#stimator = LogisticRegression(
		#	random_state=0,
		#	C=100,
		#	solver='saga'
		#
		estimator = SVC(
			random_state=0,
			kernel='sigmoid',
			degree=2,
		)

		bag = BaggingClassifier(
			random_state=0,
			base_estimator=estimator,
			n_estimators=88,
			max_samples=10000,
			bootstrap=False,
			n_jobs=22,
		)
			

		#clf = estimator
		clf = Pipeline(steps=[
			#('filter', FeatureFilter()),
			 
			#('heights', RelativeHeights()),
			#('scaler1', scaler),
			('scaler2', robustscaler),
			#('pca', PCA(n_components=16)),
			('estimator', bag),
		])

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


#inspectSample(3)

for i in range(NUM_STEPS):
	inspectSample(i)
