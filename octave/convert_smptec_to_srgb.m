function M=convert_smptec_to_srgb()
% first define the SMPTE C color space (used by MUSE)

xr1=0.63;
yr1=0.34;
xg1=0.31;
yg1=0.595;
xb1=0.155;
yb1=0.07;
xw1=0.3127;
yw1=0.3291;

C1 = make_color_matrix(xr1, yr1, xg1, yg1, xb1, yb1, xw1, yw1)

% also define the SRGB color space

xr2=0.64;
yr2=0.33;
xg2=0.30;
yg2=0.60;
xb2=0.15;
yb2=0.06;
xw2=0.3127;
yw2=0.3290;

C2 = make_color_matrix(xr2, yr2, xg2, yg2, xb2, yb2, xw2, yw2)

% To convert from the SMPTE C color space to sRGB colors, we go though
% the CIE XYZ color space

M=C2^-1 * C1;

endfunction


