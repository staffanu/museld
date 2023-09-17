% DPLL Design parameters for AC3RF
% Notice Gpd * Gvco = 1 in the implementation

Ts = 1 / 288000
% This could be good choices for g1 and g2
[ g1(1800*2*pi, 1.0, Ts) g2(1800*2*pi, 1.0, Ts) ]

% check what we get for 1/16 and 1/512 instead
f=@(x) ((g1(x(1),x(2),Ts)-1/16)^2+100*(g2(x(1),x(2),Ts)-1/512)^2)
[X, FVAL, EXITFLAG, OUTPUT] = fminsearch(f, [1000 1], optimset('TolFun', 1e-18, 'MaxIter',10000))
[ X(1)/2/pi X(2) ]

