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
ML_NAME = "net_relu_reg"

IMG_WIDTH = 1900
IMG_HEIGHT = 1000

# set current directory as working directory
os.chdir(os.path.dirname(os.path.abspath(__file__)) + "/eval")

# no preview, only load weights and store in file
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
	layers = len(weights) // 2
	for l in range(layers):
		np.save(f'{ML_NAME}_weights0_kernel{l}.npy', weights[2 * l + 0])
		np.save(f'{ML_NAME}_weights0_bias{l}.npy', weights[2 * l + 1])
	
	print("Weights:\n", weights)

	

inspectSample()