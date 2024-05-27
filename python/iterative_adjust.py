import os
import OpenEXR
import Imath
from PIL import Image
import numpy as np
#from scipy.optimize import minimize
import os

os.chdir(os.path.dirname(os.path.abspath(__file__)))

# Read UV coordinates from an EXR file
def read_exr_uv_coordinates(exr_file_path):
    exr_file = OpenEXR.InputFile(exr_file_path)
    header = exr_file.header()
    dw = header['dataWindow']
    size = (dw.max.x - dw.min.x + 1, dw.max.y - dw.min.y + 1)

    uv_r = np.frombuffer(exr_file.channel('R', Imath.PixelType(Imath.PixelType.FLOAT)), dtype=np.float32).reshape(size)
    uv_g = np.frombuffer(exr_file.channel('G', Imath.PixelType(Imath.PixelType.FLOAT)), dtype=np.float32).reshape(size)

    return np.dstack((uv_r, uv_g))

# Read a PNG image
def read_png_image(png_file_path):
    return np.array(Image.open(png_file_path).convert('RGB')).astype(np.float32) / 255.0

# Scale UV coordinates to texture atlas size
def scale_uv_coords(uv_coords, atlas_width, atlas_height):
    u_coords = uv_coords[:, :, 0] * (atlas_width - 1)
    v_coords = uv_coords[:, :, 1] * (atlas_height - 1)
    return u_coords, v_coords

# Calculate interpolation weights
def interpolation_weights(u_coords, v_coords):
    u0 = np.floor(u_coords).astype(int)
    v0 = np.floor(v_coords).astype(int)
    u1 = np.clip(u0 + 1, 0, u_coords.shape[1] - 1)
    v1 = np.clip(v0 + 1, 0, v_coords.shape[0] - 1)
    u_frac = u_coords - u0
    v_frac = v_coords - v0
    return u0, v0, u1, v1, u_frac, v_frac

# Create the final image using bilinear interpolation
def create_final_image(uv_coords, texture_atlas):
    atlas_height, atlas_width, _ = texture_atlas.shape
    u_coords, v_coords = scale_uv_coords(uv_coords, atlas_width, atlas_height)
    u0, v0, u1, v1, u_frac, v_frac = interpolation_weights(u_coords, v_coords)

    top_left = texture_atlas[v0, u0]
    top_right = texture_atlas[v0, u1]
    bottom_left = texture_atlas[v1, u0]
    bottom_right = texture_atlas[v1, u1]

    final_image = (top_left * (1 - u_frac)[:, :, np.newaxis] * (1 - v_frac)[:, :, np.newaxis] +
                   top_right * u_frac[:, :, np.newaxis] * (1 - v_frac)[:, :, np.newaxis] +
                   bottom_left * (1 - u_frac)[:, :, np.newaxis] * v_frac[:, :, np.newaxis] +
                   bottom_right * u_frac[:, :, np.newaxis] * v_frac[:, :, np.newaxis])

    return final_image

# Adjust the atlas to minimize the difference
def adjust_atlas(texture_atlas, uv_coords_list, rendered_images, learning_rate=1.0, iterations=10):
    atlas_height, atlas_width, _ = texture_atlas.shape

    for iteration in range(iterations):
        atlas_update = np.zeros_like(texture_atlas, dtype=np.float32)
        atlas_weights = np.zeros((atlas_height, atlas_width, 1), dtype=np.float32)

        curImage = 1
        for uv_coords, rendered_image in zip(uv_coords_list, rendered_images):
            reconstructed_image = create_final_image(uv_coords, texture_atlas)
            difference = rendered_image - reconstructed_image

            u_coords, v_coords = scale_uv_coords(uv_coords, atlas_width, atlas_height)
            u0, v0, u1, v1, u_frac, v_frac = interpolation_weights(u_coords, v_coords)

            weights_tl = (1 - u_frac)[:, :, np.newaxis] * (1 - v_frac)[:, :, np.newaxis]
            weights_tr = u_frac[:, :, np.newaxis] * (1 - v_frac)[:, :, np.newaxis]
            weights_bl = (1 - u_frac)[:, :, np.newaxis] * v_frac[:, :, np.newaxis]
            weights_br = u_frac[:, :, np.newaxis] * v_frac[:, :, np.newaxis]

            np.add.at(atlas_update, (v0, u0), difference * weights_tl)
            np.add.at(atlas_update, (v0, u1), difference * weights_tr)
            np.add.at(atlas_update, (v1, u0), difference * weights_bl)
            np.add.at(atlas_update, (v1, u1), difference * weights_br)

            np.add.at(atlas_weights, (v0, u0), weights_tl)
            np.add.at(atlas_weights, (v0, u1), weights_tr)
            np.add.at(atlas_weights, (v1, u0), weights_bl)
            np.add.at(atlas_weights, (v1, u1), weights_br)

            print(f"{iteration}: Processed image {curImage}/{len(uv_coords_list)}")
            curImage += 1

        # Normalize atlas_update by atlas_weights, handle division by zero
        atlas_weights = np.maximum(atlas_weights, 1e-8)
        atlas_update /= atlas_weights

        texture_atlas += learning_rate * atlas_update
        texture_atlas = np.clip(texture_atlas, 0, 1)

        print(f"Iteration {iteration + 1}/{iterations} completed.")
        # save atlas after each iteration
        save_image(texture_atlas, f"atlas_iteration_{iteration + 1}.png")

    return texture_atlas

# Save the final image and heatmap as PNG
def save_image(image_array, output_path):
    final_image = Image.fromarray((image_array * 255).astype(np.uint8))
    final_image.save(output_path)

# Main function to execute the script
def main(render_folder, uv_folder, atlas_path, output_atlas_path, iterations=10):
    print("Reading files...")
    uv_files = sorted([os.path.join(uv_folder, f) for f in os.listdir(uv_folder) if f.endswith('.exr')])
    render_files = sorted([os.path.join(render_folder, f) for f in os.listdir(render_folder) if f.endswith('.png')])

    uv_coords_list = [read_exr_uv_coordinates(uv_file) for uv_file in uv_files]
    print("uv done")
    rendered_images = [read_png_image(render_file) for render_file in render_files]
    print("render done")
    texture_atlas = read_png_image(atlas_path).astype(np.float32)
    print("atlas done")

    adjusted_atlas = adjust_atlas(texture_atlas, uv_coords_list, rendered_images, iterations=iterations)

    save_image(adjusted_atlas, output_atlas_path)

if __name__ == "__main__":
    render_folder = 'out'
    uv_folder = 'UVMaps.out0'
    atlas_path = 'textures/map0.png'
    output_atlas_path = 'textures/map0new.png'
    iterations = 1

    main(render_folder, uv_folder, atlas_path, output_atlas_path, iterations)
