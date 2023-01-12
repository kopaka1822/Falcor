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

NUM_DIRECTIONS = 8
NUM_STEPS = 4
NUM_SAMPLES = NUM_DIRECTIONS * NUM_STEPS

# set current directory as working directory
os.chdir(os.path.dirname(os.path.abspath(__file__)))

# TODO load from file?
raster = np.load('raster.npy')
ray = np.load('ray.npy')
#pixelXY = np.load('pixelXY.npy')



def inspectSample(i):
	print("length of input data: ", len(raster))

	# filter data
	filter = [r[i] > 1.0 for r in raster]
	rayf = ray[filter]
	rasterf = raster[filter]
	print("length of filtered data: ", len(rasterf))

	different = [rayf[j, i] < rasterf[j, i] for j in range(len(rayf))]

	# split rasterf and different into training and test data
	x_train, x_test, y_train, y_test = train_test_split(rasterf, different, test_size=0.2, random_state=0)

	# train decision tree classifier
	clf = tree.DecisionTreeClassifier(random_state=0)
	clf = clf.fit(x_train, y_train)

	# predict test data
	y_pred = clf.predict(x_test)
	# print accuracy
	print("accuracy: ", np.count_nonzero(y_pred == y_test) / len(y_test))

	# print confusion matrix
	print("confusion matrix:")
	print(confusion_matrix(y_test, y_pred))

	tree_importance = clf.tree_.compute_feature_importances(normalize=True)
	# write feature importances to numpy 2d array for visualization
	importance = np.zeros((9, 9))
	importance[3, 4] = tree_importance[0]
	importance[2, 4] = tree_importance[1]
	importance[1, 4] = tree_importance[2]
	importance[0, 4] = tree_importance[3]
	importance[3, 5] = tree_importance[4]
	importance[2, 6] = tree_importance[5]
	importance[1, 7] = tree_importance[6]
	importance[0, 8] = tree_importance[7]
	importance[4, 5] = tree_importance[8]
	importance[4, 6] = tree_importance[9]
	importance[4, 7] = tree_importance[10]
	importance[4, 8] = tree_importance[11]
	importance[5, 5] = tree_importance[12]
	importance[6, 6] = tree_importance[13]
	importance[7, 7] = tree_importance[14]
	importance[8, 8] = tree_importance[15]
	importance[5, 4] = tree_importance[16]
	importance[6, 4] = tree_importance[17]
	importance[7, 4] = tree_importance[18]
	importance[8, 4] = tree_importance[19]
	importance[5, 3] = tree_importance[20]
	importance[6, 2] = tree_importance[21]
	importance[7, 1] = tree_importance[22]
	importance[8, 0] = tree_importance[23]
	importance[4, 3] = tree_importance[24]
	importance[4, 2] = tree_importance[25]
	importance[4, 1] = tree_importance[26]
	importance[4, 0] = tree_importance[27]
	importance[3, 3] = tree_importance[28]
	importance[2, 2] = tree_importance[29]
	importance[1, 1] = tree_importance[30]
	importance[0, 0] = tree_importance[31]
	# save numpy array in file
	np.save("importance" + str(i) + ".npy", importance)

	#tree.plot_tree(clf)
	#plt.show()




for i in range(NUM_SAMPLES):
	inspectSample(i)
