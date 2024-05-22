# pip install requests
import os
import requests
import base64
from PIL import Image
from io import BytesIO
import threading

os.chdir(os.path.dirname(os.path.abspath(__file__)))

#api_url = "http://127.0.0.1:7860/sdapi/v1/img2img"
api_url = "http://arcade.in.tu-clausthal.de:4455/sdapi/v1/img2img"

model_id = "counterfeitV30_v30.safetensors [cbfba64e66]"
base_id = "v1-5-pruned-emaonly.safetensors [6ce0161689]"

prompt = "(masterpiece, best quality), solo, girl, xiangling, short dark blue hair, braided hair rings, yellow eyes, sleeveless, dark brown top, floral patterns, dark brown fingerless gloves, black shorts, brown yellow apron, red ribbons"
negative_prompt = "(low quality, worst quality:1.4), (bad anatomy), (inaccurate limb:1.2),bad composition, inaccurate eyes, extra digit,fewer digits,(extra arms:1.2),"
num_inference_steps = 20
cfg_scale = 1.0
simmilarity_strength = 0.61 # 1 = similar to input image

src_folder = "src"
control_folder = "lineart"
out_folder = "out"

# Ensure output directory exists
os.makedirs(out_folder, exist_ok=True)

threads = []

def post_payload(payload, filename):
    # Send the request to the API
    response = requests.post(api_url, json=payload)

    # Check the response status and save the generated image
    if response.status_code == 200:
        response_data = response.json()
        generated_image_data = response_data["images"][0]
        generated_image = Image.open(BytesIO(base64.b64decode(generated_image_data)))
        generated_image_path = os.path.join(out_folder, filename)
        generated_image.save(generated_image_path)
        #print(f"Processed {idx}/{total_files} files")
    else:
        print(f"Error {response.status_code}: {response.text}")

# Iterate over all files in the source directory
files = [f for f in os.listdir(src_folder) if f.endswith(".png")]
threads = []
total_files = len(files)
for idx, filename in enumerate(files, start=1):
    if not filename.endswith(".png"): continue
    
    source_image_path = os.path.join(src_folder, filename)

    # Load the source image
    init_image = Image.open(source_image_path).convert("RGB")
    #init_image = init_image.resize((512, 512))
    init_image.save("temp_init.png")
    # ontain size of image
    width, height = init_image.size
    
    # Prepare the control input for ControlNet (e.g., Canny edges)
    #control_path = os.path.join(control_folder + "2", filename.replace("frameGBufferRaster.diffuseOpacity", "frameGaussianBlur.dst"))
    #control_image = Image.open(control_path).convert("RGB")
    #control_image = control_image.resize((512, 512))
    #control_image.save("temp_control.png")
    
    seed = 4253

    # Prepare the payload for the API request
    payload = {
        "init_images": ["data:image/png;base64," + base64.b64encode(open("temp_init.png", "rb").read()).decode()],
        "prompt": prompt,
        "negative_prompt": negative_prompt,
        "steps": num_inference_steps,
        "cfg_scale": cfg_scale,
        "denoising_strength": 1.0 - simmilarity_strength,
        "seed": seed,
        "width": width,
        "height": height,
        "sampler_name": "DPM++ SDE",
        "scheduler": "Karras",

        #"enable_hr": True,
        #"hr_prompt": prompt,
        #"hr_negative_prompt": negative_prompt,
        #"hr_upscaler": "Latent",

        "override_settings": {
            "sd_model_checkpoint": model_id,
            "CLIP_stop_at_last_layers": 2,
        },

        "refiner_switch_at": 0.95,
        "refinder_checkpoint": base_id,

        #"controlnet_input_image": ["data:image/png;base64," + base64.b64encode(open("temp_control.png", "rb").read()).decode()],
    }

    
    # perform the rest on another thread:
    thread = threading.Thread(target=post_payload, args=(payload, filename,))
    threads.append(thread)
    thread.start()
    print(f"Started thread for {idx}/{total_files} files")
    break # only one file

for index, thread in enumerate(threads, start=1):
    thread.join()
    print(f"Thread {index}/{total_files} has finished")

