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
from sklearn.ensemble import RandomForestRegressor
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
CLEAR_FILE = True # clears cached files

#ML_NAME = f"net_{LAYERS}_{NEURONS}_"
ML_NAME = f"rnd_forest_reg"

# set current directory as working directory
os.chdir(os.path.dirname(os.path.abspath(__file__)))

ACCURACY_LOG = f'accuracy_log.txt'

def print_stats(clf, rasterf, rayf, sphereStart, sphereEnd):
	y_pred = clf.predict(rasterf)
	# print mae
	print("Median Height Error: ", np.median(np.abs(y_pred - rayf)))
	# calc ao based on sphere start and end
	#  max(sphereStart - max(sphereEnd, objectSpaceZ), 0.0);
	ao_pred = np.maximum(sphereStart - np.maximum(sphereEnd, y_pred), 0.0) / (2.0 * sphereStart)
	ao_true = np.maximum(sphereStart - np.maximum(sphereEnd, rayf), 0.0) / (2.0 * sphereStart)
	print("Mean AO Error: ", np.mean(np.abs(ao_pred - ao_true)))


def inspectSample(i):
	tf.keras.utils.set_random_seed(1) # use same random seed for training
	filename = f"{ML_NAME}{i}.pkl"

	print(f"--------------- SAMPLE {i} ------------------------------------------")

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

	# filter data for asked samples
	askedf = askedf[:, i]
	rasterf = rasterf[askedf == 1]
	rayf = rayf[:, i][askedf == 1] # only single entry for ray (the one we are trying to predict)
	sphere_startf = sphere_startf[:, i][askedf == 1]
	sphere_endf = sphere_endf[:, i][askedf == 1]

	asked_validationf = asked_validationf[:, i]
	raster_validationf = raster_validationf[asked_validationf == 1]
	ray_validationf = ray_validationf[:, i][asked_validationf == 1]
	sphere_start_validationf = sphere_start_validationf[:, i][asked_validationf == 1]
	sphere_end_validationf = sphere_end_validationf[:, i][asked_validationf == 1]

	# restrict ray labels to [-1, 1]	
	#rayf = np.clip(rayf, -1.0, 1.0)
	#ray_validationf = np.clip(ray_validationf, -1.0, 1.0)
	sphere_height = sphere_startf[0]
	# clip to sphere height
	rayf = np.clip(rayf, -sphere_height, sphere_height)
	ray_validationf = np.clip(ray_validationf, -sphere_height, sphere_height)

	print("length of data: ", len(rasterf))

	clf = None
	try:
		if CLEAR_FILE and os.path.exists(filename):
			os.remove(filename)

		clf = pickle.load(open(filename, 'rb'))
	except:
		print('no saved model found -> training new model')
		
		clf = RandomForestRegressor(
			n_estimators=96,
			random_state=0,
			bootstrap=True,
			max_samples=min(800000, len(rasterf)),
			n_jobs=-1
		)

		clf.fit(rasterf, rayf)	# also save in file

		pickle.dump(clf, open(filename, "wb"))

	# evaluate model
	print("test")
	print_stats(clf, rasterf, rayf, sphere_startf, sphere_endf)
	print("validation")
	print_stats(clf, raster_validationf, ray_validationf, sphere_start_validationf, sphere_end_validationf)
	print("----------------------------------------------------------------")


for i in range(NUM_SAMPLES):
	inspectSample(i)