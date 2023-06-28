# config
dataPath = 'D:/VAO/'

# imports
import os
import numpy as np
from PIL import Image
from tensorflow import keras
import tensorflow as tf
from keras.models import Sequential
from keras.layers import Conv2D, UpSampling2D
#from shared import AoLoss

# set current directory as working directory
os.chdir(os.path.dirname(os.path.abspath(__file__)))

tf.keras.utils.set_random_seed(3) # use same random seed for training

class AoLoss(keras.losses.Loss):
    def __init__(self, **kwargs):
        super().__init__(**kwargs)
        self.loss = keras.losses.MeanSquaredError()

    @tf.function
    def call(self, y_true, y_pred):
        y_ref = y_true[:, :, :, :, 0]
        y_max_error = y_true[:, :, :, :, 1]
        loss = self.loss(
            tf.math.multiply(y_ref, y_max_error),
            tf.math.multiply(y_pred, y_max_error)
        )
        
        return loss
    
    def get_config(self):
        config = super().get_config()
        return config

# returns true if sample was processed, false if sample was not processed because it does not exist
def process_sample(models, epochs):
    # check if f'{dataPath}bright_{sample_id}.npy' exists
    if not os.path.isfile(f'{dataPath}bright_.npy'):
        return False

    # Load and preprocess input images
    image_bright = np.load(f'{dataPath}bright_.npy')
    image_dark = np.load(f'{dataPath}dark_.npy')
    image_ref = np.load(f'{dataPath}ref_.npy')
    image_depth = np.load(f'{dataPath}depth_.npy')

    #image_invDepth = np.load(f'{dataPath}invDepth_.npy')

    # arrays have uint values 0 - 255. Convert to floats 0.0 - 1.0
    image_bright = image_bright.astype(np.float32) / 255.0
    image_dark = image_dark.astype(np.float32) / 255.0
    image_ref = image_ref.astype(np.float32) / 255.0
    image_max_error = np.maximum(image_bright - image_dark, 0.0)
    image_importance = np.ones(image_bright.shape, dtype=np.float32) - image_max_error # importance = bright - dark
    #image_importance = np.zeros(image_bright.shape, dtype=np.float32) # importance = 0

    # Preprocess images and expand dimensions
    input_data = [image_bright, image_dark, image_importance, image_depth]
    #target_data = np.expand_dims(image_ref, axis=0)
    #target_data = image_ref
    #target_data = [image_ref, image_max_error]
    target_data = tf.stack([image_ref, image_max_error], axis=4)

    for epoch in range(epochs):
        models['train'].fit(input_data, target_data, batch_size=16, initial_epoch=epoch, epochs=epoch+1)
        # save intermediate model
        for name, model in models.items():
            model.save(f'model_{name}.h5')

    return True

def build_network(prev_model = None):
    # determine size of convolutional network
    img_shape = np.load(f'{dataPath}bright_.npy').shape[1:]
    print("image shape: ", img_shape)

    activation = keras.layers.ReLU(max_value=None, negative_slope=0.0) # this relu cuts off below 0.0 and above 1.0
    #activation = 'elu'
    kernel_initializer = 'he_uniform'
    #kernel_initializer = keras.initializers.RandomUniform(minval=0.0, maxval=0.01, seed=3)
    #regularizer = keras.regularizers.l2(0.01)
    regularizer = keras.regularizers.L1L2(l1=0.0000001, l2=0.00001)

    # two inputs
    layer_input_bright = keras.layers.Input(shape=(img_shape[0], img_shape[1], 1))
    layer_input_dark = keras.layers.Input(shape=(img_shape[0], img_shape[1], 1))
    layer_input_importance = keras.layers.Input(shape=(img_shape[0], img_shape[1], 1))
    layer_input_depth = keras.layers.Input(shape=(img_shape[0], img_shape[1], 1))
    # concatenate inputs
    layer_concat = keras.layers.Concatenate(axis=-1)([layer_input_bright, layer_input_dark, layer_input_importance, layer_input_depth])
    # conv2d
    layer_conv2d_1 = keras.layers.Conv2D(8, kernel_size=3, activation=activation, kernel_initializer=kernel_initializer, kernel_regularizer=regularizer, padding='same')(layer_concat)
    layer_conv2d_2 = keras.layers.Conv2D(2, kernel_size=(7, 1), activation=activation, kernel_initializer=kernel_initializer, kernel_regularizer=regularizer, padding='same')(layer_conv2d_1)
    layer_conv2d_3 = keras.layers.Conv2D(1, kernel_size=(1, 7), activation='linear', kernel_initializer=kernel_initializer, kernel_regularizer=regularizer, padding='same')(layer_conv2d_2)
    # clamp layer between layer_input_dark and layer_input_bright
    layer_min = keras.layers.Minimum()([layer_conv2d_3, layer_input_bright])
    layer_minmax = keras.layers.Maximum()([layer_min, layer_input_dark])

    eval_model = keras.models.Model(
        inputs=[layer_input_bright, layer_input_dark, layer_input_importance, layer_input_depth],
        outputs=layer_minmax # use clamping for evaluation
    )

    train_model = keras.models.Model(
        inputs=[layer_input_bright, layer_input_dark, layer_input_importance, layer_input_depth],
        outputs=layer_conv2d_3 # use unclamped output for training
    )

    layer1_model = keras.models.Model(
        inputs=[layer_input_bright, layer_input_dark, layer_input_importance, layer_input_depth],
        outputs=layer_conv2d_1
    )

    layer2_model = keras.models.Model(
        inputs=[layer_input_bright, layer_input_dark, layer_input_importance, layer_input_depth],
        outputs=layer_conv2d_2
    )

    # Compile the model
    #models = [eval_model, train_model, layer1_model, layer2_model]
    models = {
        'eval': eval_model,
        #'train': train_model,
        'layer1': layer1_model,
        'layer2': layer2_model
    }

    for model in models.values():
        model.compile(optimizer='adam', loss='mean_squared_error')

    # special loss for training
    loss = AoLoss()
    train_model.compile(optimizer='adam', loss=loss, run_eagerly=False)
    models['train'] = train_model

    if(prev_model != None):
        # copy weights from prev_model
        train_model.set_weights(prev_model['train'].get_weights())

    return models

# use weights from prev model when training new model
models = build_network()
process_sample(models, 1000)

