from sklearn.base import BaseEstimator, TransformerMixin
import numpy as np
from tensorflow import keras
import tensorflow as tf

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
	final = np.clip(raster, -16, 16) # clip
	#final = np.concatenate((final, asked), axis=1)
	final = [final, asked]
	return final

def preprocess_ray_labels(ray, asked):
	ray = np.clip(ray, -1, 1) # clip
	# zero entries where asked is zero
	ray[asked == 0] = 0
	return ray

def vao_class_weights(askedf, requiredf):
	numRequired = np.count_nonzero(requiredf)
	numAsked = np.count_nonzero(askedf)
	numClass1 = numRequired
	numClass0 = numAsked - numRequired
	class_weight = {0: numAsked/numClass0*0.5, 1: numAsked/numClass1*0.5}
	return class_weight

class AoLoss(keras.losses.Loss):
	def __init__(self, class_weight0, class_weight1, **kwargs):
		self.class_weight0 = class_weight0
		self.class_weight1 = class_weight1
		self.crossentropy = keras.losses.BinaryCrossentropy(
			reduction=keras.losses.Reduction.NONE
		)
		super().__init__(**kwargs)
	
	@tf.function
	def call(self, y_true, y_pred):
		# calc crossentropy per timestep
		error = self.crossentropy(tf.reshape(y_true, [-1, 1]), tf.reshape(y_pred, [-1, 1]))
		# multiply errors with class weights
		w0error = error * self.class_weight0
		w1error = error * self.class_weight1
		w0error = tf.reshape(w0error, [-1, 8])
		w1error = tf.reshape(w1error, [-1, 8])
		# choose correct errors
		error = tf.where(tf.equal(y_true, 1), w1error, w0error)
		return error
		#return tf.math.reduce_mean(error)
	
	def get_config(self):
		base_config = super().get_config()
		return {**base_config, "class_weight0": self.class_weight0, "class_weight1": self.class_weight1}


# does not work...  because non differentiable?
class AoBinaryLoss(keras.losses.Loss):
	def __init__(self, class_weight0, class_weight1, **kwargs):
		self.class_weight0 = class_weight0
		self.class_weight1 = class_weight1
		super().__init__(**kwargs)
	
	def call(self, y_true, y_pred):
		# calc crossentropy per timestep
		binary_pred = tf.where(y_pred > 0.5, np.uint8(1), np.uint8(0))
		error = tf.not_equal(binary_pred, y_true)
		
		# multiply errors with class weights
		w0error = tf.where(error, self.class_weight0, 0.0)
		w1error = tf.where(error, self.class_weight1, 0.0)

		# choose correct errors
		error = tf.where(tf.equal(y_true, 1), w1error, w0error)
		return tf.reduce_mean(error)
	
	def get_config(self):
		base_config = super().get_config()
		return {**base_config, "class_weight0": self.class_weight0, "class_weight1": self.class_weight1}