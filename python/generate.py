# pip install diffusers transformers torch
#pip install accelerate
from diffusers import StableDiffusionControlNetImg2ImgPipeline, ControlNetModel, DPMSolverMultistepScheduler
from transformers import WEIGHTS_NAME, CONFIG_NAME
import torch
from PIL import Image, ImageFilter
from safetensors.torch import load_file

# set cwd to the directory of this file
import os
os.chdir(os.path.dirname(os.path.abspath(__file__)))

# Load the pre-trained model
#model_id = "CompVis/stable-diffusion-v1-4"  # You can change this to a different version if needed
model_id = "gsdf/Counterfeit-V2.0"
controlnet_model_id = "lllyasviel/control_v11p_sd15_canny" 
lora_model = None
lora_model = "lora/xiangling1-000011.safetensors"
if not torch.cuda.is_available():
    print("Warning: CUDA is not available. Please install")
    exit()

device = "cuda"

print("Device:", device)

# Set up the pipeline
controlnet = ControlNetModel.from_pretrained(controlnet_model_id)
pipe = StableDiffusionControlNetImg2ImgPipeline.from_pretrained(model_id, controlnet=controlnet)
#pipe =  StableDiffusionImg2ImgPipeline.from_pretrained(model_id)
pipe.safety_checker = None # Disable safety checker to speed up the generation process
# Set the scheduler to DPM++ SDE Karras
#pipe.controlnet = controlnet=controlnet
pipe.scheduler = DPMSolverMultistepScheduler.from_config(pipe.scheduler.config)
pipe.scheduler.set_timesteps(20)
pipe = pipe.to(device)

# Load the LoRA model
if lora_model is not None:
    lora_weights = load_file(lora_model)
    with torch.no_grad():
        for name, param in pipe.unet.named_parameters():
            if name in lora_weights:
                param.add_(lora_weights[name].to(device))
    #pipe.load_lora_weights(lora_weights)

#prompt = "((masterpiece, best quality)),a girl, solo, hat, blush,long hair, skirt, beret, sitting, bangs, socks, wariza, pink hair, light blue eyes, black headwear,holding,rifle,weapon, looking at viewer, white sailor collar, school uniform, closed mouth, black hat, sailor collar, holding weapon, long sleeves, pleated skirt, white socks,indoors,industrial"
prompt = "((masterpiece, best quality)), solo, girl, xiangling, short dark blue hair, braided hair rings, yellow eyes, sleeveless, dark brown top, floral patterns, dark brown fingerless gloves, black shorts, brown yellow apron, red ribbons"
#prompt = "((masterpiece, best quality)), Xiangling from Genshin Impact, solo, chef's attire, dark brown and yellow outfit, intricate gold details, red ribbon accents, short dark blue hair, buns, red tassels, yellow eyes, lively"
#prompt = "((masterpiece, best quality)), <lora:xiangling1-000011:1>, xianglingdef"
negative_prompt = "(low quality, worst quality:1.4), (bad anatomy), (inaccurate limb:1.2),bad composition, inaccurate eyes, extra digit,fewer digits,(extra arms:1.2),"
num_inference_steps = 20
guidance_scale = 8.0
#denoising_strength = 0.6
simmilarity_strength = 0.4 # 1 = similar to input image

# Define source and output directories
src_folder = "src"
out_folder = "out"

# Ensure output directory exists
os.makedirs(out_folder, exist_ok=True)

# Iterate over all files in the source directory
files = [f for f in os.listdir(src_folder) if f.endswith(".png")]
total_files = len(files)
for idx, filename in enumerate(files, start=1):
    if filename.endswith(".png"):
        source_image_path = os.path.join(src_folder, filename)
        
        # Load the source image
        init_image = Image.open(source_image_path).convert("RGB")
        
        # Prepare the control input for ControlNet (e.g., Canny edges)
        control_image = init_image.filter(ImageFilter.FIND_EDGES)  # Simple edge detection for demonstration
        
        seed = 4253
        generator = torch.Generator(device=device).manual_seed(seed)

        # Generate the high-resolution image with ControlNet
        with torch.autocast("cuda"):
            generated_image = pipe(prompt=prompt,
                                   negative_prompt=negative_prompt,
                                   image=init_image,
                                   control_image=control_image,
                                   strength=1.0 - simmilarity_strength,
                                   guidance_scale=guidance_scale,
                                   num_inference_steps=num_inference_steps,
                                   generator=generator).images[0]
        
        # Save the generated image
        generated_image_path = os.path.join(out_folder, filename)
        generated_image.save(generated_image_path)
        print(f"Processed {idx}/{total_files} files")
        break # only one file

print("Image generation completed!")