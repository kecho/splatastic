import coalpy.gpu as g

font_sampler = None
font_texture = None 

def init():
    global font_sampler, font_texture
    font_sampler = g.Sampler(filter_type = g.FilterType.Linear)
    font_texture = g.Texture(file = "data/debug_font.jpg")
