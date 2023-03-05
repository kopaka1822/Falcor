from sklearn.base import BaseEstimator, TransformerMixin
import numpy as np

class ClampTransformer(BaseEstimator, TransformerMixin):
	def __init__(self, value=10.0):
		self.value = value

	def fit(self, X, y=None):
		return self

	def transform(self, X, y=None):
		X = np.clip(X, -self.value, self.value) # clip
		return X

class FilterInputsTransformer(BaseEstimator, TransformerMixin):
	def __init__(self, filter = np.array([0, 1, 2, 3, 16])):
		self.filter = filter

	def fit(self, X, y=None):
		return self

	def transform(self, X, y=None):
		X = np.take(X, self.filter, axis=1)
		return X

def preprocess_inputs(raster, asked):
	final = raster.take([0, 1, 2, 3, 16], axis=1)
	final = np.clip(final, -16, 16) # clip
	#final = np.concatenate((final, asked), axis=1)
	final = [final, asked]
	return final

def preprocess_inputs_vao(raster, asked):
	final = np.clip(raster, -2, 2) # clip
	#final = np.concatenate((final, asked), axis=1)
	final = [final, asked]
	return final

def preprocess_ray_labels(ray, asked):
	ray = np.clip(ray, -1, 1) # clip
	# zero entries where asked is zero
	ray[asked == 0] = 0
	return ray