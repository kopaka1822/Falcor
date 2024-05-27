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

prompt = "(masterpiece, best quality), solo, girl, xiangling, intricate fabric, (high-definition), fine cloth details, realistic cloth folds, realistic skin details, (fine lines and wrinkles), natural skin shading, visible contours, realistic muscle,  short dark blue hair, braided hair rings, yellow eyes, sleeveless, dark brown top, floral patterns, dark brown fingerless gloves, black shorts, brown yellow apron, red ribbons"
negative_prompt = "(low quality, worst quality:1.4), (bad anatomy), (inaccurate limb:1.2),bad composition, inaccurate eyes, extra digit,fewer digits,(extra arms:1.2),"
num_inference_steps = 20
cfg_scale = 4.5
denoising_strength = 0.7 # 1 = no simmilarty, 0 = full simmilarity
seed = 4253


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
    control_path = os.path.join(control_folder + "2", filename.replace("frameGBufferRaster.diffuseOpacity", "frameGaussianBlur.dst"))
    control_image = Image.open(control_path).convert("RGB")
    #control_image = control_image.resize((512, 512))
    control_image.save("temp_control.png")
    
    control_path2 = os.path.join("depth", filename.replace("frameGBufferRaster.diffuseOpacity", "frameDepthGuide.out"))
    control_image2 = Image.open(control_path2).convert("RGB")
    #control_image = control_image.resize((512, 512))
    control_image2.save("temp_control2.png")


    # Prepare the payload for the API request
    payload = {
        "init_images": ["data:image/png;base64," + base64.b64encode(open("temp_init.png", "rb").read()).decode()],
        "prompt": prompt,
        "negative_prompt": negative_prompt,
        "steps": num_inference_steps,
        "cfg_scale": cfg_scale,
        "denoising_strength": denoising_strength,
        "seed": seed,
        "width": width,
        "height": height,
        #"sampler_name": "DPM++ 2M",
        #"scheduler": "Karras",
        "sampler_name": "DPM++ 2M Karras",

        #"enable_hr": True,
        #"hr_prompt": prompt,
        #"hr_negative_prompt": negative_prompt,
        #"hr_upscaler": "Latent",

        "override_settings": {
            "sd_model_checkpoint": model_id,
            "CLIP_stop_at_last_layers": 2,
        },

        "refiner_switch_at": 0.9,
        "refiner_checkpoint": base_id,

        #"controlnet_module": "canny",
        #"controlnet_model": "control_v11p_sd15_canny [d14c016b]",
        #"controlnet_mode": "ControlNet is more important",

        #"controlnet_input_image": ["data:image/png;base64," + base64.b64encode(open("temp_control.png", "rb").read()).decode()],

        "controlnet": {
            "mode": "ControlNet is more important",
            "enabled": True,
            "module": "canny",
            "model": "control_v11p_sd15_canny [d14c016b]",
            "pixel_perfect": False,
        },

        "alwayson_scripts": {
            "controlnet": {
                "args": [
                    {
                    "enabled": True,
                    "input_image": "data:image/png;base64," + base64.b64encode(open("temp_control.png", "rb").read()).decode(),
                    #"mask": null,
                    "module": "invert (from white bg & black line)",
                    "model": "control_v11p_sd15s2_lineart_anime [3825e83e]",
                    "weight": 1.6,
                    #"lowvram": false,
                    "processor_res": 1024,
                    #"threshold_a": 64,
                    #"threshold_b": 64,
                    "guidance_start": 0.0,
                    "guidance_end": 1.0,
                    "control_mode": "ControlNet is more important",
                    #"pixel_perfect": false
                    },
                    {
                    "enabled": True,
                    "input_image": "data:image/png;base64," + base64.b64encode(open("temp_control2.png", "rb").read()).decode(),
                    #"mask": null,
                    "module": "none",
                    "model": "control_v11f1p_sd15_depth [cfd03158]",
                    "weight": 1.6,
                    #"lowvram": false,
                    "processor_res": 1024,
                    #"threshold_a": 64,
                    #"threshold_b": 64,
                    "guidance_start": 0.0,
                    "guidance_end": 0.4,
                    "control_mode": "My prompt is more important",
                    #"pixel_perfect": false
                    },
                ]
            }
        }
    }

    
    # perform the rest on another thread:
    thread = threading.Thread(target=post_payload, args=(payload, filename,))
    threads.append(thread)
    thread.start()
    print(f"Started thread for {idx}/{total_files} files")
    #break # one file
    if len(threads) > 8:
        threads[0].join()
        threads.pop(0)

for index, thread in enumerate(threads, start=1):
    thread.join()
    #print(f"Thread {index}/{total_files} has finished")



