import numpy as np
import os
import matplotlib.pyplot as plt
from sklearn.model_selection import cross_val_score
from sklearn.linear_model import LogisticRegression
from sklearn.metrics import confusion_matrix

# set current directory as working directory
os.chdir(os.path.dirname(os.path.abspath(__file__)))

# TODO load from file?
#IMAGE_SIZE = (2048, 1061)
IMAGE_SIZE = (1061, 2048) # height x width
# load raster.npy, ray.npy and same.npy as numpy arrays
raster = np.load('raster.npy')
ray = np.load('ray.npy')
same = np.load('same.npy')
sameInstance = np.load('sameInstance.npy')
rasterInstanceDiffs = np.load('rasterInstanceDiffs.npy')
pixelXY = np.load('pixelXY.npy')

# print number of 0's in same.npy
numberValidSamples = np.count_nonzero(same) # (raster == ray)
numberInvalidSamples = len(same) - numberValidSamples # (raster != ray))
print("ray==raster for ", numberValidSamples, " samples")
print("ray!=raster for ", numberInvalidSamples, " samples")
print("score for random guessing: ", max(numberInvalidSamples, numberValidSamples) / len(same))

NUM_SAMPLES = raster.shape[1]

# transform into single samples
raster = raster.reshape(-1, 1)
ray = ray.reshape(-1, 1)
sameInstance = sameInstance.reshape(-1, 1)
rasterInstanceDiffs = rasterInstanceDiffs.reshape(-1, 1)
pixelXY = pixelXY.reshape(-1, 2)
# repeat each entry 8 times
pixelXY = np.repeat(pixelXY, NUM_SAMPLES, axis=0)

print("number of instance diff occurences [0, 1, 2]: ", np.count_nonzero(rasterInstanceDiffs == 0), np.count_nonzero(rasterInstanceDiffs == 1), np.count_nonzero(rasterInstanceDiffs == 2))



# shorten data
# remove all datapoints where raster < 1.0
ray = ray[raster > 1.0]
sameInstance = sameInstance[raster > 1.0]
rasterInstanceDiffs = rasterInstanceDiffs[raster > 1.0]
pixelXY = [pixelXY[i] for i in range(len(raster)) if raster[i] > 1.0]
raster = raster[raster > 1.0]

#same = np.logical_or((np.abs(raster - ray) < 0.05), (ray > 2.0))
same = (np.abs(raster - ray) < 0.05)

def percentOf(frac):
	return np.round(frac * 100)

print("number of samples where raster > 1.0: ", len(raster))
print("individual sample ray==raster for ", percentOf(np.count_nonzero(same)  / len(same)), "% of samples")

def printSameInstanceStats():
	sameAndSameInstance = np.logical_and(same, sameInstance)
	sameAndDifferentInstance = np.logical_and(same, np.logical_not(sameInstance))
	print("percentage of same instance samples that are ray==raster", percentOf(np.count_nonzero(sameAndSameInstance) / np.count_nonzero(sameInstance) ))
	print("percentage of different instance samples that are ray==raster", percentOf(np.count_nonzero(sameAndDifferentInstance) / np.count_nonzero(np.logical_not(sameInstance)) ))

	x = [] # raster height range
	y1 = [] # percentage of same instance samples that are ray==raster
	y2 = [] # percentage of different instance samples that are ray==raster
	for height in np.arange(1.0, 12.0, 0.1):
		x.append(height + 0.05)
		heightFilter = np.logical_and(raster >= height, raster < (height + 0.1))
		numSamplesSameInstance = max(np.count_nonzero(np.logical_and(heightFilter, sameInstance)), 1)
		numSamplesDifferentInstance = max(np.count_nonzero(np.logical_and(heightFilter, np.logical_not(sameInstance))),1)
		y1.append(np.count_nonzero(np.logical_and(heightFilter, sameAndSameInstance)) / numSamplesSameInstance * 100)
		#y1.append(np.count_nonzero(np.logical_and(heightFilter, sameAndSameInstance)))
		y2.append(np.count_nonzero(np.logical_and(heightFilter, sameAndDifferentInstance)) / numSamplesDifferentInstance * 100)
		#y2.append(np.count_nonzero(np.logical_and(heightFilter, sameAndDifferentInstance)))

	plt.plot(x, y1, 'r-', lw=2)
	plt.plot(x, y2, 'b-', lw=2)
	# name the x axis
	plt.xlabel('raster height')
	plt.ylabel('raster==ray %')
	# show a legend on the plot

	# set x ticks to 1.0
	plt.xticks(np.arange(1.0, 12.0, 1.0))
	# show gridlines in x for every tick
	plt.grid(True, which='major', axis='x')
	
	# do logistic regression based on raster heights only
	logRaster = LogisticRegression(random_state=42, n_jobs=-1, penalty='none')
	logRaster.fit(raster.reshape(-1, 1), same)
	# plot the decision function of logRaster
	xx = np.linspace(1.0, 12.0, 1000)
	# get the decision function of logRaster
	decision = logRaster.predict_proba(xx.reshape(-1, 1))
	decision = decision[:, 1] # only use second colum (same prediction)
	# plot the decision function
	plt.plot(xx, decision * 100, 'k-', lw=2)
	
	# print accuracy of logistic regression
	print("logistic regression accuracy (raster): ", logRaster.score(raster.reshape(-1, 1), same))
	
	# do logistic regrassion based on raster heights and sameInstance
	logRasterAndSameInstance = LogisticRegression(random_state=42, n_jobs=-1, penalty='none')
	logRasterAndSameInstance.fit(np.hstack((raster.reshape(-1, 1), sameInstance.reshape(-1, 1))), same)
	# plot for same instance
	decision = logRasterAndSameInstance.predict_proba(np.hstack((xx.reshape(-1, 1), np.ones((len(xx), 1)))))
	decision = decision[:, 1] # only use second colum (same prediction)
	#plt.plot(xx, decision * 100, 'g-', lw=2)
	# plot for different instance
	decision = logRasterAndSameInstance.predict_proba(np.hstack((xx.reshape(-1, 1), np.zeros((len(xx), 1)))))
	decision = decision[:, 1] # only use second colum (same prediction)
	#plt.plot(xx, decision * 100, 'y-', lw=2)

	# print accuracy of logistic regression
	print("logistic regression accuracy (raster+instance): ", logRasterAndSameInstance.score(np.hstack((raster.reshape(-1, 1), sameInstance.reshape(-1, 1))), same))

	plt.legend(['same instance', 'different instance', 'logit (raster)', 'logit (raster, sameInstance)', 'logit (raster, differentInstance)'], loc='upper right')
	plt.show()

	print("number of instance diff occurences [0, 1, 2]: ", np.count_nonzero(rasterInstanceDiffs == 0), np.count_nonzero(rasterInstanceDiffs == 1), np.count_nonzero(rasterInstanceDiffs == 2))

	# write errors of logRaster to file:
	# create numpy 2d array of size 2048 x 1061
	pixelsFalse = np.zeros(IMAGE_SIZE, dtype=np.uint8)
	pixelsCorrect = np.zeros(IMAGE_SIZE, dtype=np.uint8)
	logPredicted = logRaster.predict(raster.reshape(-1, 1))
	for i in range(len(same)):
		if logPredicted[i] != same[i]:
			pixelsFalse[pixelXY[i][1], pixelXY[i][0]] += 1
		else:
			pixelsCorrect[pixelXY[i][1], pixelXY[i][0]] += 1

	print("accuracy of raster_errors", np.count_nonzero(logPredicted == same) / len(same))

	np.save("raster_errors.npy", pixelsFalse)
	np.save("raster_correct.npy", pixelsCorrect)

	# output confusion matrix
	print("confusion matrix (raster): ", confusion_matrix(same, logPredicted)) # first row is true negative, second row is true positive, colums are predicted


printSameInstanceStats()

#while True:
# get random seed
seed = np.random.randint(0, 1000000)
# shuffle all arrays with the same seed
np.random.seed(seed)
np.random.shuffle(raster)
np.random.seed(seed)
np.random.shuffle(ray)
np.random.seed(seed)
np.random.shuffle(same)
np.random.seed(seed)
np.random.shuffle(sameInstance)
np.random.seed(seed)
np.random.shuffle(rasterInstanceDiffs)
np.random.seed(seed)
np.random.shuffle(pixelXY)

# plot raster as x-axis, sameInstance as y-axis and same as color
numElements = 10000
#plt.scatter(raster[0:numElements], same[0:numElements], c=sameInstance[0:numElements], cmap='bwr', alpha=0.1)

def setupPlot(plt):
	plt.xlabel('raster height')
	plt.ylabel('ray height')

	# restrict x-axis to [-2, 10]
	plt.xlim(0.0, 12)
	plt.ylim(0.0, 12)

	# show colorbar
	plt.colorbar()
	# set plot height
	plt.gcf().set_size_inches(12, 12)

	# plot line y=x
	plt.plot([0, 32], [0, 32], 'k-', lw=2)
	# show grid line for y=1 with gray color
	plt.axhline(y=1, color='gray', linestyle='--')
	plt.axhline(y=0, color='gray', linestyle='-')

plt.title('same instance (red) or not (blue)')
plt.scatter(raster[0:numElements], ray[0:numElements], c=sameInstance[0:numElements], cmap='bwr', alpha=0.3)
setupPlot(plt)
#plt.show()


# do a second plit with rasterInstanceDiffs as color
#plt.title('same instance (red) or not (blue)')
#plt.scatter(raster[0:numElements], ray[0:numElements], c=rasterInstanceDiffs[0:numElements], cmap='rainbow', alpha=0.3)
#setupPlot(plt)
#plt.show()
