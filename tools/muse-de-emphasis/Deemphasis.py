import numpy as np
import scipy
from scipy.optimize import fsolve
import matplotlib.pyplot as plt
from math import cos, sin

# This script computes the coefficients to make an explicit function
# for the MUSE specification of the non-linear de-emphasis filter.
# It is specified by the derivatives and values at a few points,
# and that the curve is part of an ellipse.

x1 = 224
y1 = 112
x2 = 512
y2 = 512

def equations(vars):
    h, k, a, b = vars

    xx1 = x1 - h
    xx2 = x2 - h
    yy1 = y1 - k
    yy2 = y2 - k

    eq1 = xx1 ** 2 * b ** 2 + yy1 ** 2 * a ** 2 - a ** 2 * b ** 2
    eq2 = xx2 ** 2 * b ** 2 + yy2 ** 2 * a ** 2 - a ** 2 * b ** 2
    eq3 = - b ** 2 / a ** 2 * xx1 / yy1 - 0.5
    eq4 = - b ** 2 / a ** 2 * xx2 / yy2 - 2.5

    return [eq1, eq2, eq3, eq4]


h, k, a, b = fsolve(equations, [100, 600, 500, 1000])
print(f"h: {h}, k: {k}, a: {a}, b: {b}, f: {equations([h, k, a, b])}")

plt.figure()

alpha = np.linspace(0, 2 * np.pi, 100)
ell_x = h + a * np.cos(alpha)
ell_y = k + b * np.sin(alpha)
plt.plot(ell_x, ell_y)

x = np.linspace(x1, x2, 50)
y = k - np.sqrt(b ** 2 * (1 - (x - h) ** 2 / a ** 2))
plt.plot(x, y)
x = np.linspace(-x1, x1, 50)
y = x / 2
plt.plot(x, y)
x = np.linspace(-x2, -x1, 50)
y = -k  + np.sqrt(b ** 2 * (1 - (-x - h) ** 2 / a ** 2))
plt.plot(x, y)

plt.scatter(x1, y1)
plt.scatter(x2, y2)

plt.axis('equal')
plt.show()
