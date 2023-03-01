import numpy as np
import os

# combines from 0 to (NUM_FILES - 1)
FIRST_FILE = 0
LAST_FILE = 31

# set current directory as working directory
os.chdir(os.path.dirname(os.path.abspath(__file__)))
cleanupFiles = []

def combineFiles(baseName):
	filenames = []
	for i in range(FIRST_FILE, LAST_FILE + 1):
		fname = f'{baseName}{i}.npy'
		filenames.append(fname)
		cleanupFiles.append(fname)
	
	combined = np.concatenate([np.load(filename) for filename in filenames])
	np.save(f'{baseName}.npy', combined)

#v1
#for i in range(4):
#	combineFiles(f'raster_train{i}_')
#	combineFiles(f'ray_train{i}_')
#	combineFiles(f'required_train{i}_')
#	combineFiles(f'weight_train{i}_')

combineFiles('raster_train_')
combineFiles('ray_train_')
combineFiles('required_train_')
combineFiles('required_forced_train_')
combineFiles('asked_train_')

# cleanup the old files
for filename in cleanupFiles:
	os.remove(filename)