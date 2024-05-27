# pip install OpenEXR Imath Pillow numpy
import OpenEXR
import Imath
from PIL import Image
import numpy as np
import os

os.chdir(os.path.dirname(os.path.abspath(__file__)))

exr_file_path = 'UVMaps.out0/frameUVMaps.out000001.exr'
png_file_path = 'textures/map0new.png'
output_path = 'uvtest0.png'
usage_path = 'usage.png'

# Open the EXR file and read the UV coordinates
def read_exr_uv_coordinates(exr_file_path):
    exr_file = OpenEXR.InputFile(exr_file_path)
    header = exr_file.header()
    dw = header['dataWindow']
    size = (dw.max.x - dw.min.x + 1, dw.max.y - dw.min.y + 1)

    # Read the UV coordinates from the EXR file (assuming they are stored in R and G channels)
    uv_r = np.frombuffer(exr_file.channel('R', Imath.PixelType(Imath.PixelType.FLOAT)), dtype=np.float32).reshape(size)
    uv_g = np.frombuffer(exr_file.channel('G', Imath.PixelType(Imath.PixelType.FLOAT)), dtype=np.float32).reshape(size)

    return np.dstack((uv_r, uv_g))

# Read the PNG texture atlas
def read_texture_atlas(png_file_path):
    texture_atlas = Image.open(png_file_path).convert('RGB')
    return np.array(texture_atlas)

# Create the final image by replacing UV coordinates with corresponding texture pixels
def create_final_image(uv_coords, texture_atlas):
    height, width, _ = uv_coords.shape
    atlas_height, atlas_width, _ = texture_atlas.shape

    # Initialize the final image array
    final_image = np.zeros((height, width, 3), dtype=np.uint8)

    # Scale UV coordinates to texture atlas size
    u_coords = np.clip((uv_coords[:, :, 0] * atlas_width).astype(int), 0, atlas_width - 1)
    v_coords = np.clip((uv_coords[:, :, 1] * atlas_height).astype(int), 0, atlas_height - 1)

    # Calculate the coordinates of the top-left corner
    u0 = np.floor(u_coords).astype(int)
    v0 = np.floor(v_coords).astype(int)

    # Calculate the coordinates of the bottom-right corner
    u1 = np.clip(u0 + 1, 0, atlas_width - 1)
    v1 = np.clip(v0 + 1, 0, atlas_height - 1)

    # Calculate the fractional part
    u_frac = u_coords - u0
    v_frac = v_coords - v0

    # Fetch the four surrounding pixels
    top_left = texture_atlas[v0, u0]
    top_right = texture_atlas[v0, u1]
    bottom_left = texture_atlas[v1, u0]
    bottom_right = texture_atlas[v1, u1]

    # Perform bilinear interpolation
    final_image = (top_left * (1 - u_frac)[:, :, np.newaxis] * (1 - v_frac)[:, :, np.newaxis] + \
                   top_right * u_frac[:, :, np.newaxis] * (1 - v_frac)[:, :, np.newaxis] + \
                   bottom_left * (1 - u_frac)[:, :, np.newaxis] * v_frac[:, :, np.newaxis] + \
                   bottom_right * u_frac[:, :, np.newaxis] * v_frac[:, :, np.newaxis])
    #final_image = texture_atlas[v_coords, u_coords] # Nearest neighbor interpolation


    # Track usage of atlas pixels
    #usage = np.zeros((atlas_height, atlas_width), dtype=np.float32)
    #for y in range(height):
    #    for x in range(width):
    #        usage[v0[y, x], u0[y, x]] += 1
    #        usage[v0[y, x], u1[y, x]] += 1
    #        usage[v1[y, x], u0[y, x]] += 1
    #        usage[v1[y, x], u1[y, x]] += 1

    return final_image.astype(np.uint8)#, usage

# Save the final image as PNG
def save_image(image_array, output_path):
    final_image = Image.fromarray(image_array)
    final_image.save(output_path)

uv_coords = read_exr_uv_coordinates(exr_file_path)
texture_atlas = read_texture_atlas(png_file_path)
#final_image, usage = create_final_image(uv_coords, texture_atlas)
final_image = create_final_image(uv_coords, texture_atlas)
save_image(final_image, output_path)
#save_image((usage).astype(np.uint8), usage_path)