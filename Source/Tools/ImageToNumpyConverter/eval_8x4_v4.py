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

IMG_WIDTH = 1900
IMG_HEIGHT = 1000

# set current directory as working directory
os.chdir(os.path.dirname(os.path.abspath(__file__)))

raster = np.load('raster0.npy')
ray = np.load('ray0.npy')
sphereStart = np.load('sphereStart0.npy')
pixelXY = np.load('pixelXY0.npy')
print("length of input data: ", len(raster))

output_predict = np.zeros((NUM_SAMPLES, IMG_HEIGHT, IMG_WIDTH), dtype=np.uint8) # 0 = raster, 1 = ray
output_correct = np.zeros((NUM_SAMPLES, IMG_HEIGHT, IMG_WIDTH), dtype=np.uint8)
output_all = np.zeros((NUM_SAMPLES, IMG_HEIGHT, IMG_WIDTH), dtype=np.uint8)

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

	print(f"--------- SAMPLE {i} -------------------------------------------------")

	print("loading classifier...")
	filename = f"rnd_forest_{i}.pkl"
	clf = pickle.load(open(filename, 'rb'))

	print("filtering data...")
	# filter data
	filter = [raster[j, i] > sphereStart[j, i] for j in range(len(raster))]
	rasterf = raster[filter]
	rayf = ray[filter]
	pixelf = pixelXY[filter]

	# store all candidates that were in question
	for pixel in pixelf:
		output_all[i, pixel[1], pixel[0]] = 1
	
	# store all candidates that are actually ray
	y_correct = [rayf[j, i] < rasterf[j, i] for j in range(len(rayf))]
	for pixel in pixelf[y_correct]:
		output_correct[i, pixel[1], pixel[0]] = 1

	# filter input parameters
	rasterf = rasterf[:, getSampleMask(i)]

	print("predicting...")
	# predict all test data
	y_pred = clf.predict(rasterf)

	# store all candidates that were predicted to be ray
	for pixel in pixelf[y_pred]:
		output_predict[i, pixel[1], pixel[0]] = 1

	# print accuracy
	print("accuracy: ", np.count_nonzero(y_correct == y_pred) / len(y_correct))
	# print confusion matrix
	print("confusion matrix:")
	print(confusion_matrix(y_correct, y_pred))

for i in range(NUM_SAMPLES):
	inspectSample(i)

# save image
np.save('y_predict.npy', output_predict)
np.save('y_correct.npy', output_correct)
np.save('y_all.npy', output_all)