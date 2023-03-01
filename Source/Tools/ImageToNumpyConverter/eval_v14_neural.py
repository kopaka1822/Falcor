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
from shared import preprocess_inputs_vao
import tensorflow_model_optimization as tfmot

import tensorflow as tf
from tensorflow import keras

NUM_SAMPLES = 8
EVAL_ID = 0
ML_NAME = "net_relu_pruned"

IMG_WIDTH = 1900
IMG_HEIGHT = 1000

# set current directory as working directory
os.chdir(os.path.dirname(os.path.abspath(__file__)) + "/eval")


output_correct = np.zeros((NUM_SAMPLES, IMG_HEIGHT, IMG_WIDTH), dtype=np.uint8)
output_asked = np.zeros((NUM_SAMPLES, IMG_HEIGHT, IMG_WIDTH), dtype=np.uint8)
output_probability = np.zeros((NUM_SAMPLES, IMG_HEIGHT, IMG_WIDTH), dtype=np.float32)

def print_weights(weights):
	kernel0 = weights[0]
	bias0 = weights[1]
	kernel1 = weights[2]
	bias1 = weights[3]
	kernel2 = weights[4]
	bias2 = weights[5]
	print("kernel0", ','.join(map(str, kernel0.flatten())))
	print("bias0", ','.join(map(str, bias0.flatten())))
	print("kernel1", ','.join(map(str, kernel1.flatten())))
	print("bias1", ','.join(map(str, bias1.flatten())))
	print("kernel2", ','.join(map(str, kernel2.flatten())))
	print("bias2", ','.join(map(str, bias2.flatten())))
		
# filter data
rasterf = np.load(f'raster_eval_{EVAL_ID}.npy')
requiredf = np.load(f'required_eval_{EVAL_ID}.npy')
askedf = np.load(f'asked_eval_{EVAL_ID}.npy')
pixelf = np.load(f'pixelXY_{EVAL_ID}.npy')

# preprocess data
print("preprocessing data")
rasterf = preprocess_inputs_vao(rasterf, askedf)

def inspectSample():

	filename = f"../{ML_NAME}.pkl"
	if not os.path.exists(filename):
		print("model not found")
		
	clf = pickle.load(open(filename, 'rb'))
	
	net = clf
	net.summary()

	# get weights of the first layer
	weights = net.get_weights()
	#print_weights(weights)

	np.save(f'{ML_NAME}_weights0_kernel0.npy', weights[0])
	np.save(f'{ML_NAME}_weights0_bias0.npy', weights[1])
	np.save(f'{ML_NAME}_weights0_kernel1.npy', weights[2])
	np.save(f'{ML_NAME}_weights0_bias1.npy', weights[3])
	#np.save(f'{ML_NAME}_weights0_kernel2.npy', weights[4])
	#np.save(f'{ML_NAME}_weights0_bias2.npy', weights[5])
	print("Weights:\n", weights)

	print("writing y_asked")
	# store all candidates that were in question
	#for i in range(len(pixelf)):
	for step in range(NUM_SAMPLES):	
		askedff = askedf[:,step] # filter for step index
		pixelff = pixelf[askedff == 1] # filter for asked pixels
		for pixel in pixelff:
			output_asked[step, pixel[1], pixel[0]] = 1

	print("writing y_correct")
	# store all candidates that are actually ray
	for step in range(NUM_SAMPLES):
		requiredff = requiredf[:,step]
		pixelff = pixelf[requiredff == 1]
		for pixel in pixelff:
			output_correct[step, pixel[1], pixel[0]] = 1

	print("predicting...")
	# predict all test data
	y_pred = net.predict(rasterf, batch_size = 1024)


	#y_cpred = custom_predict(weights, rasterf)
	#print("y_pred == y_cpred: ", np.isclose(y_pred, y_cpred, atol=0.001, rtol=0.001).all())

	print("writing y_predict")
	for step in range(NUM_SAMPLES):
		y_predf = y_pred[:,step]
		for i in range(len(pixelf)):
			output_probability[step, pixelf[i][1], pixelf[i][0]] = y_predf[i]

inspectSample()

# save image
output_predict = np.where(np.logical_and(output_probability > 0.5, output_asked == 1), 1, 0)

np.save('y_predict.npy', output_predict)
np.save('y_correct.npy', output_correct)
np.save('y_asked.npy', output_asked)
np.save('y_proba.npy', output_probability)

# compute output forced
output_forced = np.zeros((NUM_SAMPLES, IMG_HEIGHT, IMG_WIDTH), dtype=np.uint8)

forced = np.load(f'required_forced_eval_{EVAL_ID}.npy')
for j in range(len(forced)):
	p = pixelf[j]
	for i in range(NUM_SAMPLES):
		if forced[j][i] == 1:
			output_forced[i, p[1], p[0]] = 1

np.save('y_forced.npy', output_forced)