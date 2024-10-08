from falcor import *

def render_graph_Perlin():
    g = RenderGraph('Perlin')
    g.create_pass('PerlinNoise', 'PerlinNoise', {})
    g.mark_output('PerlinNoise.outPerlinTex')
    return g

Perlin = render_graph_Perlin()
try: m.addGraph(Perlin)
except NameError: None
