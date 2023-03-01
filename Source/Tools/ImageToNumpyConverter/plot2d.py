import matplotlib as mpl
from matplotlib import pyplot
import numpy as np

def sigmoid(x):
    return 1.0 / (1.0 + np.exp(-x))

def net(inputs):
    layer0Output0 = -1.5775
    layer0Output1 = -1.84917
    layer0Output2 = 3.5325
    layer0Output3 = -0.703624
    layer0Output4 = -0.525449
    layer0Output5 = -1.83955
    layer0Output6 = 0.2325
    layer0Output7 = -0.790567
    layer0Output0 += 3.0972 * inputs[4]
    layer0Output0 += 0.045998 * inputs[6]
    layer0Output1 += 2.53289 * inputs[7]
    layer0Output2 += -0.465067 * inputs[0]
    layer0Output2 += -0.0712934 * inputs[2]
    layer0Output5 += 2.13896 * inputs[3]
    layer0Output6 += -0.0316677 * inputs[4]
    layer0Output6 += 2.27759 * inputs[5]
    layer0Output0 = max(layer0Output0, 0.0)
    layer0Output1 = max(layer0Output1, 0.0)
    layer0Output2 = max(layer0Output2, 0.0)
    layer0Output3 = max(layer0Output3, 0.0)
    layer0Output4 = max(layer0Output4, 0.0)
    layer0Output5 = max(layer0Output5, 0.0)
    layer0Output6 = max(layer0Output6, 0.0)
    layer0Output7 = max(layer0Output7, 0.0)

    layer1Output0 = 5.7399 # depends on 0, 2
    layer1Output1 = 0.464897 #no dependence, always true
    layer1Output2 = 1.32792 # depends on 0, 2
    layer1Output3 = -2.44211 # depends on 3
    layer1Output4 = -1.7426 # depends on 4, 6
    layer1Output5 = -4.76137 # depends on 4, 5
    layer1Output6 = 0.786217 # depends on 7, 0, 2
    layer1Output7 = -2.97069 # depends on 7
    layer1Output0 += -2.11201 * layer0Output2
    layer1Output2 += -0.30936 * layer0Output2
    layer1Output3 += 1.4867 * layer0Output5
    layer1Output4 += 0.491306 * layer0Output0
    layer1Output5 += 1.18083 * layer0Output6
    layer1Output6 += 0.181417 * layer0Output1
    layer1Output6 += -0.164514 * layer0Output2
    layer1Output7 += 1.55289 * layer0Output1

    return sigmoid(layer1Output4)


def plot(ax):
    # make values from -5 to 5, for this example
    sizex = 320
    sizey = 320
    zvals = np.zeros((sizey, sizex), dtype=np.float32)
    inputs = np.zeros(8, dtype=np.float32)
    for H6 in range(sizey):
        for H4 in range(sizex):
            inputs[4] = H4/10 - 16
            inputs[6] = H6/10 - 16
            zvals[H6][H4] = net(inputs)

    # tell imshow about color map so that only set colors are used
    img = ax.imshow(
        zvals,
        interpolation='nearest',
        cmap = 'inferno',
        origin='lower',
        extent=[-16, 16, -16, 16]
    )

    # make a color bar
    pyplot.colorbar(img, cmap='inferno', ax=ax)
    # set range of color bar
    #img.set_clim(0, 20)
    # set x label
    #ax.set_title('H16 = ' + str(H16))

fig, ax = pyplot.subplots(1)
plot(ax)
#plot(ax[1], -8)
#plot(ax[2], -16)
#plot(ax, 0.0)

pyplot.ylabel('H6')
pyplot.xlabel('H4')
pyplot.show()