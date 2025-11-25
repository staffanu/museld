import lcapy
from lcapy import j2pif, Circuit, LTIFilter
from matplotlib import pyplot
import sympy as sp

# schematics taken from CLD-D504-EN-Service-Manual_Scan.pdf
# and CLD-D780-EN_Service_Manual_Scan.pdf

filter_netlist = """
    R1 in 2 1500; right
    W 0 0_1; right
    C2 2 0_1 100e-12; down=3
    W 2 2_2; up
    L3 2 3 180e-9; right
    C4 2_2 3_1 7e-12; right
    W 3 3_1; up
    L5 3 4 180e-9; right
    C6 3_1 4_1 22e-12; right
    W 4 4_1; up
    W 0_1 0_3; right
    C7 4 0_3 82e-12; down
    W 4 4_3; right
    W 0_3 0_4; right
    R8 4_3 0_4 1500; down
    C9 4_3 B1_1 47e-9; right

    W 0_4 B0_1; right

    R10 B1_1 B0_1 10e3; down
    R11 B1_1 B2 1e3; right
    E12 B4 0 opamp B2 B3 1e5
    W B3 B3_1; down
    W B4 B4_1; down=1.5
    R13 B4_1 B3_1 10e3; left
    R14 B4 B5 50e3; right
    W B4 B4_2; up
    W B5 B5_1; up
    C15 B4_2 B5_1 100e-12; right
    R16 B5 Bout 3.3e3; right
    W B0_1 D0_2; right=5

    W Bout Din; right
    
    W Din D1_2; down
    R17 D1_2 D2 33e3; right
    R18 Din D3 33e3; right
    R19 D2 D4 1e3; right
    C20 D2 D0_2 0.022e-6; down=1.5
    R21 D3 D5 1e3; right
    E22 D6 D0 opamp D4 D5 1e5; flipud
    W D6 D6_1; right
    W D6_1 D6_2; up
    R23 D6_2 D7 68e3; left
    W D7 D7_1; up
    R24 D7 D5_1 470e3; left
    C25 D7_1 D5_2 100e-12; left
    W D5_2 D5_1; down
    W D5_1 D5; down
    W D6_1 out; right
    W D0_2 D0_8; right=5
"""

filter_circuit = lcapy.Circuit(filter_netlist)
filter_circuit.draw()

filter_transfer = filter_circuit.transfer("in", 0, "out", 0)
#print(filter_transfer.ZPK().pretty())
filter_transfer(j2pif).bode_plot((1e4, 10e6), phase='degrees', dbmin=-40)
pyplot.show()

H_s = sp.simplify(sp.sympify(str(filter_transfer)))
print(H_s)

Fs = 24.583333e6
Ts = 1.0 / Fs
z = sp.symbols('z')
s_sym = sp.symbols('s')
s_to_z = (2.0 / Ts) * (1 - z**-1) / (1 + z**-1)
H_z = sp.simplify(H_s.subs(s_sym, s_to_z))

z_inv = sp.symbols('z_inv')
H_z_inv = sp.simplify(H_z.subs(z**-1, z_inv))

num, den = sp.fraction(H_z_inv)
num_poly = sp.Poly(num, z_inv)
den_poly = sp.Poly(den, z_inv)

b = [float(c) for c in reversed(num_poly.all_coeffs())]  # [b0, b1, ...]
a = [float(c) for c in reversed(den_poly.all_coeffs())]  # [a0, a1, ...]

a0 = a[0]
b = [bi / a0 for bi in b]
a = [ai / a0 for ai in a]
print("b=", b)
print("a=", a)
