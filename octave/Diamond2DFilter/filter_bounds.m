b=@(x,y,d) abs(x)+abs(y) + d < pi/2;

pass=@(x,y) b(x,y,pi/32); # 1 to pass, 0 to block, a bit bigger than optimal
block=@(x,y) 1-b(x,y,-pi/32); # 1 to block, 0 to pass, a bit bigger than optimal

lb=@(x,y) (1-pass(x,y))*-0.02+pass(x,y)*0.98;
ub=@(x,y) block(x,y)*0.02+(1-block(x,y))*1.02;

global n=47;
global x y xx yy;
x=y=linspace(-pi + pi / n, pi - pi/n, n);
[xx, yy]=meshgrid(x, y);
global LB=arrayfun(lb, xx, yy);
global UB=arrayfun(ub, xx, yy);

