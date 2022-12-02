import numpy as np
import os
import matplotlib.pyplot as plt
from sklearn.model_selection import cross_val_score
import xgboost as xgb
from scipy.fft import dct, idct

# set current directory as working directory
os.chdir(os.path.dirname(os.path.abspath(__file__)))

# load raster.npy, ray.npy and same.npy as numpy arrays
raster = np.load('raster.npy')
ray = np.load('ray.npy')
same = np.load('same.npy')

# print number of 0's in same.npy
numberValidSamples = np.count_nonzero(same) # (raster == ray)
numberInvalidSamples = len(same) - numberValidSamples # (raster != ray))
print("ray==raster for ", numberValidSamples, " samples")
print("ray!=raster for ", numberInvalidSamples, " samples")
print("score for random guessing: ", max(numberInvalidSamples, numberValidSamples) / len(same))

def fitLegendre(plt, xvalues, yvalues):
	#poly = np.polynomial.legendre.Legendre.fit(xvalues, yvalues, 3)
	poly = np.polynomial.legendre.Legendre.fit(xvalues, yvalues, deg=[0, 2, 4, 6])
	# higher resolution for plotting
	morexvalues = np.linspace(0, 1, 100)
	moreyvalues = poly(morexvalues)
	plt.plot(morexvalues, moreyvalues, label='legendre', color='blue')

def fitDCT(plt, xvalues, yvalues):
	original_coeff = dct(yvalues, norm='ortho')
	new_coeff = np.zeros(100)
	# copy all original_coeff to new_coeff
	new_coeff[:len(original_coeff)] = original_coeff
	newyvalues = idct(new_coeff, norm='ortho')
	plt.plot(np.linspace(0, 1, 100), newyvalues, label='dct', color='blue')


def plot_sample(index):
	raster_sample = raster[index]
	ray_sample = ray[index]
	# add first element to the end of the array (since it is a closed loop)
	raster_sample = np.append(raster_sample, raster_sample[0])
	ray_sample = np.append(ray_sample, ray_sample[0])

	# create derivative of raster_sample
	raster_sample_derivative = [raster_sample[(i + 1)] - raster_sample[i - 1] for i in range(len(raster_sample) - 1)]
	ray_sample_derivative = [ray_sample[(i + 1)] - ray_sample[i - 1] for i in range(len(ray_sample) - 1)]

	# clamp negative values
	raster_sample[raster_sample < -1] = -1
	ray_sample[ray_sample < -1] = -1
	# clamp positive values
	raster_sample[raster_sample > 10] = 10
	ray_sample[ray_sample > 10] = 10

	# prepare two plots
	fig, (ax1, ax2) = plt.subplots(1, 2)
	# set plot size
	fig.set_size_inches(10.5, 6.5)

	uniformxvalues = np.linspace(0, 1, len(raster_sample))

	# plot raster_sampple as line plot
	ax1.plot(uniformxvalues, ray_sample, label='ray', color='red')
	ax1.plot(uniformxvalues, raster_sample, label='raster', color='green')
	#fitLegendre(ax1, uniformxvalues, raster_sample)
	#fitDCT(ax1, uniformxvalues, raster_sample)

	uniformxvalues = np.linspace(0, 1, len(raster_sample_derivative))
	ax2.plot(uniformxvalues, ray_sample_derivative, label='ray deriv', color='red')
	ax2.plot(uniformxvalues, raster_sample_derivative, label='raster deriv', color='green')

	# show legend
	ax1.legend()
	ax2.legend()
	plt.show(block=True)
	
print("data processing")

# data processing:
# clamp raster samples to [-2, 2]
#raster[raster < -2] = -2
#raster[raster > 2] = 2

## create sample vector with 1.0 as value
#compare_vec = np.zeros(len(raster[0]))
#compare_vec.fill(1.0)
## fillter indices where raster[i] < compare_vec
#filtered_raster = raster[np.where(np.count_nonzero(raster > compare_vec, axis=1) <= 1)]
#filtered_same = same[np.where(np.count_nonzero(raster > compare_vec, axis=1) <= 1)]
#print("number of instances where 1 sample is ambigous: ", len(filtered_same))
#numberValidSamples = np.count_nonzero(filtered_same) # (raster == ray)
#numberInvalidSamples = len(filtered_same) - numberValidSamples # (raster != ray))
#print("ray==raster for ", numberValidSamples, " samples")
#print("ray!=raster for ", numberInvalidSamples, " samples")
#raster = filtered_raster
#same = filtered_same


print("cross validation")

## find first entry where same is 0
notSameIndices = np.where(same == 0)[0]
sameIndices = np.where(same == 1)[0]
# shuffle indices with seed 42
np.random.seed(42)
np.random.shuffle(notSameIndices)
np.random.shuffle(sameIndices)

##plot_sample(firstNotSameIndex)
#for idx in notSameIndices:
#for idx in sameIndices:
	#plot_sample(idx)

# random forest cross val scores: 0.75295726 0.51554261 0.60914481 0.74363825 0.84142549
#from sklearn.ensemble import RandomForestClassifier
#clf = RandomForestClassifier(random_state=42)
#scores = cross_val_score(clf, raster, same, cv=5, n_jobs=-1)
#print(scores)

# logistic regression cross val scores: 0.54536344 0.60718734 0.60718734 0.60718734 0.60719303
#from sklearn.linear_model import LogisticRegression
#clf = LogisticRegression(random_state=42)
#scores = cross_val_score(clf, raster, same, cv=5, n_jobs=-1)
#print(scores)

# NearestCentroid: 0.55996478 0.56151015 0.59686619 0.59386912 0.59139271
#from sklearn.neighbors import NearestCentroid
#clf = NearestCentroid()
#scores = cross_val_score(clf, raster, same, cv=5, n_jobs=-1)
#print(scores)

# neural net:           0.68139289 0.71085782 0.84122093 0.87849697 0.82143861
# neural net: (clamped) 0.71791029 0.72778189 0.78679604 0.84135205 0.82555961
#from sklearn.neural_network import MLPClassifier
#clf = MLPClassifier(random_state=42)
#scores = cross_val_score(clf, raster, same, cv=5, n_jobs=-1)
#print(scores)

# gradient boost:           0.76351256 0.84836707 0.87874985 0.86225661 0.82104524
# gradient boost: (clamped) 0.77073363 0.84444278 0.87948038 0.86089856 0.81748619
#from sklearn.ensemble import GradientBoostingClassifier
#clf = GradientBoostingClassifier(random_state=42)
#scores = cross_val_score(clf, raster, same, cv=5, n_jobs=-1)
#print(scores)

# xgb: 0.75607609 0.63822574 0.76325032 0.85327477 0.83257469
#clf = xgb.XGBClassifier(random_state=42, objective='binary:logistic')
#scores = cross_val_score(clf, raster, same, cv=5, n_jobs=-1)
#print(scores)

#from catboost import CatBoostClassifier
#clf = CatBoostClassifier(random_state=42, verbose=False)
#scores = cross_val_score(clf, raster, same, cv=5)
#print(scores)