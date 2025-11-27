import lcapy
from lcapy import j2pif, Circuit, LTIFilter
from matplotlib import pyplot
import sympy as sp

# schematics taken from CLD-D504-EN-Service-Manual_Scan.pdf
# and CLD-D780-EN_Service_Manual_Scan.pdf

filter_d504_netlist = """
    R1 in 2 1500; right
    W 0 0_1; right
    C2 2 0_1 100e-12; down=3
    W 2 2_2; up
    
    # W 2 3; right
    L3 2 3 180e-6; right
    C4 2_2 3_1 7e-12; right
        
    W 3 3_1; up
    L5 3 4 180e-6; right
    C6 3_1 4_1 22e-12; right
    W 4 4_1; up
    W 0_1 0_3; right
    C7 4 0_3 82e-12; down
    W 4 4_3; right
    W 0_3 0_4; right
    R8 4_3 0_4 1500; down
    C9 4_3 5 47e-9; right
    W 0_4 0_5; right
    
    # from here below is the filter inside the DSP, the same as used in, e.g., the X9
    R10 5 0_5 10e3; down
    R11 5 6 1e3; right
    E12 8 0 opamp 6 7_1 1e5
    W 7_1 7; down
    W 8 8_1; down=1.5
    R13 8_1 7 10e3; left
    R14 8 9 50e3; right
    W 8 8_2; up
    W 9 9_1; up
    C15 8_2 9_1 100e-12; right
    R16 9 10 3.3e3; right
    W 0_5 0_8; right=5

    W 10 10_1; down
    R17 10_1 11 33e3; right
    R18 10 12 33e3; right
    R19 11 13 1e3; right
    C20 11 0_8 0.022e-6; down=1.5
    R21 12 14 1e3; right
    E22 15 0 opamp 13 14 1e5; flipud
    W 15 15_1; right
    W 15_1 15_2; up
    R23 15_2 16 68e3; left
    W 16 16_1; up
    R24 16 14_1 470e3; left
    C25 16_1 14_2 100e-12; left
    W 14_2 14_1; down
    W 14_1 14; down
    W 15_1 out; right
    W 0_8 0_9; right=5.5
"""

# input filter from the HLD-X9.  The same DSP is used as in the D504 after this.
filter_x0_netlist = """
   R1 in 2 1e3; right
   C2 2 0_2 22e-12; down
   W 0 0_2; right
   L3 2 3 62e-6; right
   C4 3 0_3 82e-12; down
   W 0_2 0_3; right
   L5 3 4 62e-6; right
   C6 4 0_4 22e-12; down
   W 4 5;right
   W 0_3 0_4; right
   R7 5 0_5 1e3; down
   W 0_4 0_5; right
   W 5 out; right
"""

# This is the filter from IC501 (p. 51 of the service manual) of the Sony HIL-C1, called "RF EQ".
# RFH / RFL inputs -- set RFH to 0 if using RFL
filter_hil_c1_netlist = """
    #W RFH 0; down 
    W 0 0_1; right
    E1 3 0 opamp RFH 2 10000;
    W 2 2_1; down
    R2 2_1 5 47e3; right
    R3 3 5 11e3; down=1.5
    W 2_1 2_2; down
    R4 2_2 4 8200; down
    C5 4 0 47e-12; down
    C6 2_2 5_1 1.7e-12; right
    W 5 5_1; down
    R7 5_1 0_2 20e3; down
    W 0_1 0_2; right
    W 3 6; right
    R8 6 7 15e3; down
    R9 7 RFL 30e3; down
    W 7 7_1; right
    E10 9 0 opamp 8 7_1 10000; flipud
    R11 8 0_3 8.2e3; down
    W 0_2 0_3; right 
    R12 9 10 20e3; right
    W 9 9_1; up=1.5
    W 9_1 9_2; up
    W 7_1 7_2; up
    W 7_2 7_3; up
    R13 9_1 7_2 47e3; left
    C14 7_3 9_2 0.6e-12; right
    E15 12 0 opamp 11 10 10000; flipud
    R16 11 0_4 10e3; down
    W 0_3 0_4; right
    W 12 12_1; up=1.5
    W 10 10_1; up
    R17 10_1 12_1 20e3; right
    W 12_1 12_2; up
    W 10_1 10_2; up
    C18 10_2 12_2 0.6p; right
    W 12 out; right
"""
# The first part of the EQ filter -- the second part doesn't do much, maybe it is just an amplifier?
filter_hil_c1_rf_eq_netlist = """
    W 0 0_1; right
    E1 3 0 opamp in 2 10000;
    W 2 2_1; down
    R2 2_1 5 47e3; right
    R3 3 5 11e3; down=1.5
    W 2_1 2_2; down
    R4 2_2 4 8200; down
    C5 4 0 47e-12; down
    C6 2_2 5_1 1.7e-12; right
    W 5 5_1; down
    R7 5_1 0_2 20e3; down
    W 0_1 0_2; right
    W 3 6; right
    R8 6 out 10e3; down
    R9 out 0_3 1.1e3; down
    W 0_2 0_3; right
"""

filter_circuit = lcapy.Circuit(filter_hil_c1_rf_eq_netlist)
filter_circuit.draw()

filter_transfer = filter_circuit.transfer("in", 0, "out", 0)
#print(filter_transfer.ZPK().pretty())
filter_transfer(j2pif).bode_plot((1e4, 10e6), phase='degrees', dbmin=-40)
pyplot.show()

H_s = sp.simplify(sp.sympify(str(filter_transfer)))
print(H_s)

Fs = sp.symbols('Fs') # 40e6 # 24.583333e6
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

print("Numerator: ", num_poly.all_coeffs())
print("Denominator: ", den_poly.all_coeffs())

#b = [float(c) for c in reversed(num_poly.all_coeffs())]  # [b0, b1, ...]
#a = [float(c) for c in reversed(den_poly.all_coeffs())]  # [a0, a1, ...]
b = [c for c in reversed(num_poly.all_coeffs())]  # [b0, b1, ...]
a = [c for c in reversed(den_poly.all_coeffs())]  # [a0, a1, ...]

a0 = a[0]
b = [bi / a0 for bi in b]
a = [ai / a0 for ai in a]
print("b=", b)
print("a=", a)
