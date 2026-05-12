import matplotlib.pyplot as plt
from math import sqrt

# A simple verification that the coefficients obtained by Deemphasis.py
# work and produce reasonable results in the form coded in the de-emphasis
# shader.  Notice that the range is different from that script since the
# specification uses 11 bit resolution of the filter, and we use the value
# range 0-255.  We use floats, however, so there is no loss of resolution.

# Verbatim copy from the shader after syntax fixes for Python
def non_linear(x_unscaled):
    nl_y1 = 112
    nl_x1 = 224
    nl_x2 = 512
    nl_y2 = 512
    nl_h = 96.65
    nl_k = 964.23
    nl_a = 483.50
    nl_a_sq = nl_a * nl_a
    nl_b = 883.43
    nl_b_sq = nl_b * nl_b

    x = (x_unscaled - 128) * 2;
    if (x > nl_x2):
        y = nl_y2
    elif (x > nl_x1):
        y = nl_k - sqrt(nl_b_sq * (1 - (x - nl_h) * (x - nl_h) / nl_a_sq))
    elif (x < -nl_x2):
        y = -nl_y2
    elif (x < -nl_x1):
        y = -nl_k + sqrt(nl_b_sq * (1 - (-x - nl_h) * (-x - nl_h) / nl_a_sq))
    else:
        y = x / 2;

    return y + 128

# Notice that the actual excess range used on laserdisc is much smaller than for BS MUSE
max_range = 160
t = range(128-max_range, 128+max_range, 1)
plt.plot(t, list(map(non_linear, t)))
plt.show()

step = 10
for i in range(128-max_range, 128+max_range + step - 1, 10):
    print(i, non_linear(i))
