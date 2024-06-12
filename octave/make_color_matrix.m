% Creates the matrix to transform RGB values into CIE XYZ values.
% Inputs are the RGB primaries and the white point xy values.
function C=make_color_matrix(xr, yr, xg, yg, xb, yb, xw, yw)

zr=1-xr-yr;
zg=1-xg-yg;
zb=1-xb-yb;
zw=1-xw-yw;

M = [xr xg xb; yr yg yb; zr zg zb];

a = M^-1 * [xw/yw; 1; zw/yw];

C = [a';a';a'] .* M;

endfunction


