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

import tensorflow as tf
from tensorflow import keras

print("TensorFlow version: ", tf.__version__)
print("Num GPUs Available: ", len(tf.config.list_physical_devices('GPU')))
# hide gpu for cpu usage
tf.config.set_visible_devices([], 'GPU')

NUM_DIRECTIONS = 8
NUM_STEPS = 4
NUM_SAMPLES = NUM_DIRECTIONS * NUM_STEPS
EVAL_ID = 0
OUTPUT_PROBA = True
ML_NAME = "net_relu"

IMG_WIDTH = 1900
IMG_HEIGHT = 1000

# set current directory as working directory
os.chdir(os.path.dirname(os.path.abspath(__file__)) + "/eval")

output_predict = np.zeros((NUM_SAMPLES, IMG_HEIGHT, IMG_WIDTH), dtype=np.uint8) # 0 = raster, 1 = ray
output_correct = np.zeros((NUM_SAMPLES, IMG_HEIGHT, IMG_WIDTH), dtype=np.uint8)
output_all = np.zeros((NUM_SAMPLES, IMG_HEIGHT, IMG_WIDTH), dtype=np.uint8)
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


def custom_predict(weights, X):
	kernel0 = weights[0]
	bias0 = weights[1]
	kernel1 = weights[2]
	bias1 = weights[3]
	kernel2 = weights[4]
	bias2 = weights[5]

	Y = np.zeros(X.shape[0], dtype=np.float32)

	for xIdx in range(X.shape[0]):
		# scale inputs
		x = np.take(X[xIdx], [0, 1, 2, 3, 16], axis=0)
		x = np.clip(x, -16, 16) # clip

		layer1Output = bias0.copy()

		# layer 1 calculation
		for i in range(5):
			for j in range(5):
				layer1Output[i] += kernel0[j][i] * x[j]

		# appy relu
		for i in range(5):
			layer1Output[i] = max(0, layer1Output[i])


		layer2Output = bias1.copy()

		# layer 2 calculation
		for inIdx in range(5):
			for outIdx in range(5):
				layer2Output[outIdx] += kernel1[inIdx][outIdx] * layer1Output[inIdx]

		# appy relu
		for i in range(5):
			layer2Output[i] = max(0, layer2Output[i])
		
		layer3Output = bias2.copy()
		for inIdx in range(5):
			layer3Output[0] += kernel2[inIdx][0] * layer2Output[inIdx]

		# apply sigmoid
		layer3Output[0] = 1 / (1 + np.exp(-layer3Output[0]))
		Y[xIdx] = layer3Output[0]

		print(f"\r{xIdx} / {X.shape[0]}", end="")
	print("")
	return Y
		

def inspectSample(i):

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

	filename = f"../{ML_NAME}{i}.pkl"
	if not os.path.exists(filename):
		print("model not found")
		
	clf = pickle.load(open(filename, 'rb'))
	
	net = clf.named_steps['net']
	net.summary()

	# get weights of the first layer
	weights = net.get_weights()
	print_weights(weights)
	return

	np.save(f'{ML_NAME}_weights{i}_kernel0.npy', weights[0])
	np.save(f'{ML_NAME}_weights{i}_bias0.npy', weights[1])
	np.save(f'{ML_NAME}_weights{i}_kernel1.npy', weights[2])
	np.save(f'{ML_NAME}_weights{i}_bias1.npy', weights[3])
	np.save(f'{ML_NAME}_weights{i}_kernel2.npy', weights[4])
	np.save(f'{ML_NAME}_weights{i}_bias2.npy', weights[5])
	#print("Weights:\n", weights)
	

	print("predicting...")
	# predict all test data
	y_pred = clf.predict(rasterf).reshape(-1)


	#y_cpred = custom_predict(weights, rasterf)
	#print("y_pred == y_cpred: ", np.isclose(y_pred, y_cpred, atol=0.001, rtol=0.001).all())

	if OUTPUT_PROBA:
		for j in range(len(y_pred)):
			output_probability[pixelf[j][2], pixelf[j][1], pixelf[j][0]] = y_pred[j]

	# convert probabilities to integers
	y_pred = np.where(y_pred > 0.5, 1, 0)

	# store all candidates that were predicted to be ray
	for j in range(len(y_pred)):
		output_predict[pixelf[j][2], pixelf[j][1], pixelf[j][0]] = y_pred[j]

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
if OUTPUT_PROBA:
	np.save('y_proba.npy', output_probability)

# compute output forced
output_forced = np.zeros((NUM_SAMPLES, IMG_HEIGHT, IMG_WIDTH), dtype=np.uint8)
forced = np.load(f'forcedXY_eval_{EVAL_ID}.npy')
for pixel in forced:
	output_forced[pixel[2], pixel[1], pixel[0]] = 1
np.save('y_forced.npy', output_forced)