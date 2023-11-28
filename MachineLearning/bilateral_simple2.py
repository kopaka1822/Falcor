# config
#dataPaths = ['D:/VAO/park', 'D:/VAO/city']
dataPaths = ['D:/VAO/park']
fileEnding = '.npy'

# imports
import os
import numpy as np
from tensorflow import keras
import tensorflow as tf
from keras.models import Sequential
from keras.layers import Conv2D, UpSampling2D
from shared import *
import math

# set current directory as working directory
os.chdir(os.path.dirname(os.path.abspath(__file__)))

tf.keras.utils.set_random_seed(2) # use same random seed for training



class PrintWeightsCallback(tf.keras.callbacks.Callback):
    def on_epoch_end(self, epoch, logs=None):
        for variable in self.model.trainable_variables:
            variable_name = variable.name
            variable_value = variable.numpy()
            print(f" {variable_name}: {variable_value}", sep=',', end='')
        print()

class SSIMLoss(keras.losses.Loss):
    def __init__(self, **kwargs):
        super().__init__(**kwargs)

    @tf.function
    def call(self, y_true, y_pred):
        y_ref = y_true[:, :, :, :, 0]
        y_max_error = y_true[:, 5:-5, 5:-5, :, 1]

        dssim = 1.0 - tf.image.ssim(y_ref, y_pred, max_val=1.0, return_index_map=True, filter_size=11)

        return dssim * tf.squeeze(y_max_error, axis=-1)
    
    def get_config(self):
        config = super().get_config()
        return config

class GradientLoss(keras.losses.Loss):
    def __init__(self, **kwargs):
        super().__init__(**kwargs)

    @tf.function
    def call(self, y_true, y_pred):
        y_ref = y_true[:, :, :, :, 0]
        y_max_error = y_true[:, :, :, :, 1]

        # calculate gradient
        y_pred_gradient = tf.image.sobel_edges(y_pred)
        y_ref_gradient = tf.image.sobel_edges(y_ref)

        y_gradient_error = tf.abs(y_pred_gradient - y_ref_gradient)
        y_gradient_error = tf.squeeze(y_gradient_error, axis=[3]) # create 4D tensor with 2 channels
        
        y_value_error = tf.abs(y_pred - y_ref)

        y_loss = tf.square(y_value_error) + tf.square(y_gradient_error)
        y_loss = y_loss * y_max_error # prioritize loss in error prone areas

        return y_loss


# returns true if sample was processed, false if sample was not processed because it does not exist
def process_sample(model : keras.models.Model, epochs):
    # check if f'{dataPath}bright_{sample_id}.npy' exists
    image_bright = None
    image_dark = None
    image_ref = None
    image_depth = None

    for dataPath in dataPaths:
        if not os.path.isfile(f'{dataPath}bright_{fileEnding}'):
            return False

        # Load and preprocess input images
        ib = np.load(f'{dataPath}bright_{fileEnding}')
        id = np.load(f'{dataPath}dark_{fileEnding}')
        ir = np.load(f'{dataPath}ref_{fileEnding}')
        id = np.load(f'{dataPath}depth_{fileEnding}')
        if image_bright is None:
            image_bright = ib
            image_dark = id
            image_ref = ir
            image_depth = id
        else:
            image_bright = np.concatenate((image_bright, ib), axis=0)
            image_dark = np.concatenate((image_dark, id), axis=0)
            image_ref = np.concatenate((image_ref, ir), axis=0)
            image_depth = np.concatenate((image_depth, id), axis=0)
    
    image_bright = image_bright.astype(np.float32) / 255.0
    image_dark = image_dark.astype(np.float32) / 255.0
    image_ref = image_ref.astype(np.float32) / 255.0
    image_max_error = np.maximum(image_bright - image_dark, 0.0)

    # Preprocess images and expand dimensions
    input_data = [image_bright, image_dark, image_depth]
    target_data = tf.stack([image_ref, image_max_error], axis=4)
    #target_data = image_ref


    model_checkpoint_callback = tf.keras.callbacks.ModelCheckpoint(
        filepath='model_checkpoint.h5',
        save_weights_only=True,
        monitor='loss',
        mode='min',
        save_best_only=True,
        verbose=1,
    )

    models['train'].fit(input_data, target_data, batch_size=16, epochs=epochs, callbacks=[PrintWeightsCallback(), model_checkpoint_callback])

    # load best model wheits
    models['train'].load_weights('model_checkpoint.h5')
    
    # print final weights
    print("BEST WEIGHTS: ------------------")
    for variable in models['train'].trainable_variables:
        variable_name = variable.name
        variable_value = variable.numpy()
        print(f" {variable_name}: {variable_value}", sep=',', end='')
    print()

    return True

def build_network():
    # determine size of convolutional network
    img_shape = np.load(f'{dataPaths[0]}bright_{fileEnding}').shape[1:]
    print("image shape: ", img_shape)

    # two inputs
    layer_input_bright = keras.layers.Input(shape=(img_shape[0], img_shape[1], 1))
    layer_input_dark = keras.layers.Input(shape=(img_shape[0], img_shape[1], 1))
    layer_input_depth = keras.layers.Input(shape=(img_shape[0], img_shape[1], 1))
    
    bilateral = BilateralBlur(R=2)
    bilateral_layer = bilateral([layer_input_bright, layer_input_dark, layer_input_depth])

    train_model = keras.models.Model(
        inputs=[layer_input_bright, layer_input_dark, layer_input_depth],
        outputs=bilateral_layer
    )

    eval_model = keras.models.Model(
        inputs=[layer_input_bright, layer_input_dark, layer_input_depth],
        outputs=bilateral_layer
    )

    models = {
        'eval': eval_model,
    }

    for model in models.values():
        # add a callback function that prints the weights after each epoch
        model.compile(optimizer='adam', loss='mean_squared_error')

    # special loss for train model
    loss = AoLoss()
    #loss = GradientLoss()
    #loss = SSIMLoss()
    #loss = 'mean_squared_error'
    optimizer = keras.optimizers.Adam(learning_rate=0.001)
    #optimizer = keras.optimizers.Nadam()
    #optimizer = keras.optimizers.SGD(learning_rate=0.001, momentum=0.9, nesterov=True)
    train_model.compile(optimizer=optimizer, loss=loss, run_eagerly=False)
    models['train'] = train_model

    return models


models = build_network()
#models['train'].load_weights('model_checkpoint.h5')
process_sample(models, 6000)
for name, model in models.items():
    model.save(f'model_{name}.h5')