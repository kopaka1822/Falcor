import matplotlib as mpl
from matplotlib import pyplot
import numpy as np

def net(H0, H3, H16):
    N1 = max(0, -2.27 * H16 - 0.06)
    N2 = max(0, 2.82 * H0 -0.58)
    N3 = max(0, -1.76 * H0 +0.86 * H3 +0.65)
    N4 = max(0, -2.33 * H16 +0.36)

    F2 = max(0, 1.57 * N1 + 0.68 * N2 - 1.68 * N3 -1.67 * N4 + 2.87)
    return F2

def plot(ax, H16):
    # make values from -5 to 5, for this example
    size = 17
    zvals = np.zeros((size, size), dtype=np.float32)
    for H0 in range(size):
        for H3 in range(size):
            zvals[H0][H3] = net(H0, H3, H16)

    # tell imshow about color map so that only set colors are used
    img = ax.imshow(
        zvals,
        interpolation='nearest',
        cmap = 'inferno',
        origin='lower')

    # make a color bar
    pyplot.colorbar(img, cmap='inferno', ax=ax)
    # set range of color bar
    img.set_clim(0, 20)
    # set x label
    ax.set_title('H16 = ' + str(H16))

fig, ax = pyplot.subplots(3)
plot(ax[0], 0)
plot(ax[1], -8)
plot(ax[2], -16)

pyplot.xlabel('H3')
pyplot.ylabel('H0')
pyplot.show()