import numpy as np
import os

# combines from 0 to (NUM_FILES - 1)
FIRST_FILE = 0
LAST_FILE = 7

# set current directory as working directory
os.chdir(os.path.dirname(os.path.abspath(__file__)))

def combineFiles(baseName):
	filenames = []
	for i in range(FIRST_FILE, LAST_FILE + 1):
		filenames.append(f'{baseName}{i}.npy')
	
	combined = np.concatenate([np.load(filename) for filename in filenames])
	np.save(f'{baseName}.npy', combined)

combineFiles('raster')
combineFiles('ray')
combineFiles('sphereStart')