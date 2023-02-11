import matplotlib as mpl
from matplotlib import pyplot
import numpy as np

def net(H0, H1, H3, H16):
    N0 = max(0, -2.36 * H0 + 1.12 * H1 +1.2)
    N1 = max(0, -2.27 * H16 - 0.06)
    N2 = max(0, 2.82 * H0 -0.58)
    N3 = max(0, -1.76 * H0 +0.86 * H3 +0.65)
    N4 = max(0, -2.33 * H16 +0.36)

    F0 = max(0, 3.13 * N1 -3.09 * N4 +1.06)
    F1 = max(0, -0.59 * N1 - 1.34 * N2 + 0.59 * N4 + 3.02) # negative influence
    F2 = max(0, 1.57 * N1 + 0.68 * N2 - 1.68 * N3 -1.67 * N4 + 2.87)
    F3 = max(0, 0.37 * N0 + 0.69 * N1 + 1.36 * N2 - 0.43 * N4 - 0.06)
    F4 = max(0, -0.93 * N2-0.24 * N4 - 0.21) 
    #F4 = -0.93 * N2-0.24 * N4 - 0.21
    return F4

def plot(ax):
    # make values from -5 to 5, for this example
    sizex = 320
    sizey = 320
    zvals = np.zeros((sizey, sizex), dtype=np.float32)
    for H0 in range(sizey):
        for H16 in range(sizex):
            zvals[H0][H16] = net(H0/10-16, 0.0, 0.0, H16/10 - 16)

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

pyplot.xlabel('H16')
pyplot.ylabel('H0')
pyplot.show()