# pip install OpenEXR Imath Pillow numpy
import os
import OpenEXR
import Imath
from PIL import Image
import numpy as np
import os
import threading

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

# Read a PNG image with alpha channel
def read_png_image_with_alpha(png_file_path):
    return np.array(Image.open(png_file_path).convert('RGBA')).astype(np.float32) / 255.0

# Scale UV coordinates to texture atlas size
def scale_uv_coords(uv_coords, atlas_width, atlas_height):
    u_coords = np.clip(np.floor(uv_coords[:, :, 0] * atlas_width).astype(int), 0, atlas_width - 1)
    v_coords = np.clip(np.floor(uv_coords[:, :, 1] * atlas_height).astype(int), 0, atlas_height - 1)
    return u_coords, v_coords

# Create the final image using nearest neighbor interpolation
def create_final_image(u_coords, v_coords, texture_atlas):
    final_image = texture_atlas[v_coords, u_coords]
    return final_image

# Adjust the atlas to minimize the difference
def adjust_atlas(texture_atlas, scaled_uv_coords_list, rendered_images, learning_rate=1.0, iterations=10, threshold=0.1):
    atlas_height, atlas_width, _ = texture_atlas.shape
    # Separate the alpha channel
    alpha_channel = texture_atlas[:, :, 3].copy()
    texture_atlas_rgb = texture_atlas[:, :, :3]

    for iteration in range(iterations):
        atlas_update = np.zeros_like(texture_atlas_rgb, dtype=np.float32)
        atlas_weights = np.zeros((atlas_height, atlas_width, 1), dtype=np.float32)

        for idx, (u_coords, v_coords) in enumerate(scaled_uv_coords_list):
            rendered_image = rendered_images[idx]
            reconstructed_image = create_final_image(u_coords, v_coords, texture_atlas_rgb)
            difference = rendered_image - reconstructed_image

            #diff_dot = 256 * np.dot(difference, [0.33, 0.33, 0.33])
            #weights = 1 / (1 + diff_dot**2)

            # Calculate the custom weights based on the threshold
            diff_dot = np.dot(difference, [0.33, 0.33, 0.33])
            weights = np.where(diff_dot > threshold, 0, 1)

            # Use numpy advanced indexing and broadcasting to update atlas
            np.add.at(atlas_update, (v_coords, u_coords), difference * weights[:, :, np.newaxis])
            np.add.at(atlas_weights, (v_coords, u_coords),  weights[:, :, np.newaxis])

            print(f"Processed image index: {idx}/{len(scaled_uv_coords_list)}.")

        # Normalize atlas_update by atlas_weights, handle division by zero
        atlas_weights = np.maximum(atlas_weights, 1e-8)
        atlas_update /= atlas_weights

        texture_atlas_rgb += learning_rate * atlas_update
        texture_atlas_rgb = np.clip(texture_atlas_rgb, 0, 1)

        print(f"Iteration {iteration + 1}/{iterations} completed.")

    # Recombine the RGB channels with the alpha channel
    adjusted_atlas = np.dstack((texture_atlas_rgb, alpha_channel))
    return adjusted_atlas

# Save the final image with alpha channel as PNG
def save_image_with_alpha(image_array, output_path):
    final_image = Image.fromarray((image_array * 255).astype(np.uint8), 'RGBA')
    final_image.save(output_path)

# Main function to execute the script
def main(render_folder, uv_folder, atlas_path, output_atlas_path, iterations=10):
    uv_files = sorted([os.path.join(uv_folder, f) for f in os.listdir(uv_folder) if f.endswith('.exr')])
    render_files = sorted([os.path.join(render_folder, f) for f in os.listdir(render_folder) if f.endswith('.png')])

    texture_atlas = read_png_image_with_alpha(atlas_path).astype(np.float32)
    atlas_height, atlas_width, _ = texture_atlas.shape

    scaled_uv_coords_list = [scale_uv_coords(read_exr_uv_coordinates(uv_file), atlas_width, atlas_height) for uv_file in uv_files]
    rendered_images = [read_png_image_with_alpha(render_file)[:, :, :3] for render_file in render_files]  # Exclude alpha from rendered images

    adjusted_atlas = adjust_atlas(texture_atlas, scaled_uv_coords_list, rendered_images, iterations=iterations)

    save_image_with_alpha(adjusted_atlas, output_atlas_path)

def run_threaded_iteration(i):
    render_folder = 'out'
    uv_folder = f'UVMaps.out{i}'
    atlas_path = f'textures/map{i}.png'
    output_atlas_path = f'textures/map{i}new.png'
    iterations = 1

    main(render_folder, uv_folder, atlas_path, output_atlas_path, iterations)

if __name__ == "__main__":
    threads = []
    for i in range(4):
        thread = threading.Thread(target=run_threaded_iteration, args=(i,))
        threads.append(thread)
        thread.start()

    for thread in threads:
        thread.join()
