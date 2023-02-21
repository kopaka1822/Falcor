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
from sklearn.base import BaseEstimator, TransformerMixin

import tensorflow as tf
from tensorflow import keras

NUM_SAMPLES = 8
EVAL_ID = 0
ML_NAME = "forest"

IMG_WIDTH = 1900
IMG_HEIGHT = 1000

# set current directory as working directory
os.chdir(os.path.dirname(os.path.abspath(__file__)) + "/eval")

output_predict = np.zeros((NUM_SAMPLES, IMG_HEIGHT, IMG_WIDTH), dtype=np.uint8) # 0 = raster, 1 = ray
output_correct = np.zeros((NUM_SAMPLES, IMG_HEIGHT, IMG_WIDTH), dtype=np.uint8)
output_all = np.zeros((NUM_SAMPLES, IMG_HEIGHT, IMG_WIDTH), dtype=np.uint8)

# load evaluation data
raster_validation = np.load(f'raster_eval_0.npy')
required_validation = np.load(f'required_eval_0.npy')
asked_validation = np.load(f'asked_eval_0.npy')
pixel = np.load(f'pixelXY_0.npy')
forced = np.load(f'required_forced_eval_0.npy')

# process data (clip)
raster_validation = np.clip(raster_validation, -16, 16)

def inspectSample(stepIndex):
	filename = f"../{ML_NAME}{stepIndex}.pkl"
	print(f"--------- SAMPLE {stepIndex} -------------------------------------------------")

	# filter results (only take the stepIndex column)
	required_validationf = required_validation[:, stepIndex]
	asked_validationf = asked_validation[:, stepIndex]

	# only take samples where asked is 1
	rasterf = raster_validation[asked_validationf == 1]
	requiredf = required_validationf[asked_validationf == 1]
	pixelf = pixel[asked_validationf == 1]
	#forcedf = forced[asked_validationf == 1]

	# store all candidates that were in question
	for p in pixelf:
		output_all[stepIndex, p[1], p[0]] = 1

	# store all candidates that are actually ray
	for j in range(len(requiredf)):
		output_correct[stepIndex, pixelf[j][1], pixelf[j][0]] = requiredf[j]

	clf = pickle.load(open(filename, 'rb'))

	print("predicting...")
	# predict all test data
	y_pred = clf.predict(rasterf)

	# store all candidates that were predicted to be ray
	for j in range(len(y_pred)):
		output_predict[stepIndex, pixelf[j][1], pixelf[j][0]] = y_pred[j]

	# print accuracy
	print("accuracy: ", np.count_nonzero(requiredf == y_pred) / len(y_pred))
	# print confusion matrix
	print("confusion matrix:")
	conf_mat = confusion_matrix(requiredf, y_pred)
	print(conf_mat)
	print("Recall: ", conf_mat[1][1] / (conf_mat[1][1] + conf_mat[1][0]))

for i in range(NUM_SAMPLES):
	inspectSample(i)

# save image
np.save('y_predict.npy', output_predict)
np.save('y_correct.npy', output_correct)
np.save('y_all.npy', output_all)

# compute output forced
output_forced = np.zeros((NUM_SAMPLES, IMG_HEIGHT, IMG_WIDTH), dtype=np.uint8)

forced = np.load(f'required_forced_eval_{EVAL_ID}.npy')
for j in range(len(forced)):
	p = pixel[j]
	for i in range(NUM_SAMPLES):
		if forced[j][i] == 1:
			output_forced[i, p[1], p[0]] = 1

np.save('y_forced.npy', output_forced)