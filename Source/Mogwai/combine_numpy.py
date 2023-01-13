import numpy as np
import os

# combines from 0 to (NUM_FILES - 1)
NUM_FILES = 8

# set current directory as working directory
os.chdir(os.path.dirname(os.path.abspath(__file__)))

def combineFiles(baseName, numFiles):
	filenames = []
	for i in range(numFiles):
		filenames.append(f'{baseName}{i}.npy')
	
	combined = np.concatenate([np.load(filename) for filename in filenames])
	np.save(f'{baseName}.npy', combined)

combineFiles('raster', NUM_FILES)
combineFiles('ray', NUM_FILES)