from sklearn.base import BaseEstimator, TransformerMixin
import numpy as np

class ClampTransformer(BaseEstimator, TransformerMixin):
	def __init__(self, value=10.0):
		self.value = value

	def fit(self, X, y=None):
		return self

	def transform(self, X, y=None):
		X = np.clip(X, -self.value, self.value) #/ self.value # clip and normalize
		return X

class FilterInputsTransformer(BaseEstimator, TransformerMixin):
	def __init__(self, filter = np.array([0, 1, 2, 3, 16])):
		self.filter = filter

	def fit(self, X, y=None):
		return self

	def transform(self, X, y=None):
		X = np.take(X, self.filter, axis=1)
		return X