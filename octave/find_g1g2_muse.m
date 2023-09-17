% DPLL Design parameters for the MUSE baseband signal
%
%

Ts = 54 / 16.2
w = 60 * 2 * pi / 54e6
z = 1
% This could be good choices for g1 and g2
g_vec = [ g1(w, z, Ts) g2(w, z, Ts) ]
G_vec = g_vec / 9216

