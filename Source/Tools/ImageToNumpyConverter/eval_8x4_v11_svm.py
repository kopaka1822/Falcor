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

NUM_DIRECTIONS = 8
NUM_STEPS = 4
NUM_SAMPLES = NUM_DIRECTIONS * NUM_STEPS
EVAL_ID = 0
OUPTU_PROBA = False
ML_NAME = "svc_bag"

IMG_WIDTH = 1900
IMG_HEIGHT = 1000

# set current directory as working directory
os.chdir(os.path.dirname(os.path.abspath(__file__)) + "/eval")

output_predict = np.zeros((NUM_SAMPLES, IMG_HEIGHT, IMG_WIDTH), dtype=np.uint8) # 0 = raster, 1 = ray
output_correct = np.zeros((NUM_SAMPLES, IMG_HEIGHT, IMG_WIDTH), dtype=np.uint8)
output_all = np.zeros((NUM_SAMPLES, IMG_HEIGHT, IMG_WIDTH), dtype=np.uint8)
output_probability = np.zeros((NUM_SAMPLES, IMG_HEIGHT, IMG_WIDTH), dtype=np.float32)

class ClampTransformer(BaseEstimator, TransformerMixin):
	def __init__(self):
		self.value = 10.0

	def fit(self, X, y=None):
		return self

	def transform(self, X, y=None):
		X = np.clip(X, -self.value, self.value)
		return X

def inspectSample(i):
	filename = f"../{ML_NAME}{i}.pkl"
	print(f"--------- SAMPLE {i} -------------------------------------------------")

	# filter data
	rasterf = np.load(f'raster_eval{i}_{EVAL_ID}.npy')
	requiredf = np.load(f'required_eval{i}_{EVAL_ID}.npy').reshape(-1)
	pixelf = np.load(f'pixelXY{i}_{EVAL_ID}.npy')

	# store all candidates that were in question
	for pixel in pixelf:
		output_all[pixel[2], pixel[1], pixel[0]] = 1

	# store all candidates that are actually ray
	for j in range(len(requiredf)):
		output_correct[pixelf[j][2], pixelf[j][1], pixelf[j][0]] = requiredf[j]

	clf = pickle.load(open(filename, 'rb'))

	print("predicting...")
	# predict all test data
	y_pred = clf.predict(rasterf)

	# store all candidates that were predicted to be ray
	for j in range(len(y_pred)):
		output_predict[pixelf[j][2], pixelf[j][1], pixelf[j][0]] = y_pred[j]

	if OUPTU_PROBA:
		y_proba = clf.predict_proba(rasterf)
		for j in range(len(y_proba)):
			output_probability[pixelf[j][2], pixelf[j][1], pixelf[j][0]] = y_proba[j][1]

	# print accuracy
	print("accuracy: ", np.count_nonzero(requiredf == y_pred) / len(y_pred))
	# print confusion matrix
	print("confusion matrix:")
	conf_mat = confusion_matrix(requiredf, y_pred)
	print(conf_mat)
	print("Recall: ", conf_mat[1][1] / (conf_mat[1][1] + conf_mat[1][0]))

for i in range(NUM_STEPS):
	inspectSample(i)

# save image
np.save('y_predict.npy', output_predict)
np.save('y_correct.npy', output_correct)
np.save('y_all.npy', output_all)
if OUPTU_PROBA:
	np.save('y_proba.npy', output_probability)

# compute output forced
output_forced = np.zeros((NUM_SAMPLES, IMG_HEIGHT, IMG_WIDTH), dtype=np.uint8)
forced = np.load(f'forcedXY_eval_{EVAL_ID}.npy')
for pixel in forced:
	output_forced[pixel[2], pixel[1], pixel[0]] = 1
np.save('y_forced.npy', output_forced)