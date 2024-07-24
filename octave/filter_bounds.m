b=@(x,y,d) abs(x)+abs(y) + d < pi/4;

pass=@(x,y) b(x,y,pi/16); # 1 to pass, 0 to block, a bit bigger than optimal
block=@(x,y) 1-b(x,y,-pi/16); # 1 to block, 0 to pass, a bit bigger than optimal

lb=@(x,y) (1-pass(x,y))*-0.03+pass(x,y)*0.97;
ub=@(x,y) block(x,y)*0.03+(1-block(x,y))*1.03;

n=63;
x=y=linspace(-pi + pi / n, pi - pi/n, n);
[xx, yy]=meshgrid(x, y);
LB=arrayfun(lb, xx, yy);
UB=arrayfun(ub, xx, yy);

