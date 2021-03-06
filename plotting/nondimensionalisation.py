
def dimensional_time(t, length_scale=1.0):
    # length_scale = 1.0
    heat_diffusivity = 1.25e-7

    timescale = length_scale ** 2 / heat_diffusivity  # timescale in seconds
    timescale = timescale / 3600  # convert timescale to hours
    timescale = timescale / 24  # convert to days

    dim_t = t * timescale

    return dim_t


def dimensional_salinity(s, c_ref=230.0, delta_c=200.0):
    dim_s = c_ref + delta_c * s
    return dim_s


def dimensional_temperature(temperature, reference_temperature=-23.0, delta_t=20.0):
    dim_temperature = temperature * delta_t + reference_temperature

    return dim_temperature


def dimensional_velocity(vel, length_scale=1.0):
    heat_diffusivity = 1.25e-7
    vel_scale = heat_diffusivity/length_scale

    vel = vel*vel_scale

    return vel
