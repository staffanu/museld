% requires:
% pkg load miscellaneous
% pkg load image

prefix = "test_picture"
%prefix = "fish"
%prefix="intro"
%prefix="ice"
%prefix="fly"

img0=imread(["../data/muse/" prefix "_field_00003-0.png"]); % current frame
img1=imread(["../data/muse/" prefix "_field_00002-0.png"]); % one frame back
img2=imread(["../data/muse/" prefix "_field_00001-0.png"]); % two frames back

M0=double(img0(:,:,1));

M1=double(img1(:,:,1));
M2=double(img2(:,:,1));

H=[0 -1 0; -1 4 -1; 0 -1 0];

function B=unquincunx(A)
  if A(1,1) == 0
    Mx=A(1:2:end,2:2:end);
    My=A(2:2:end,1:2:end);
  else
    Mx=A(1:2:end,1:2:end);
    My=A(2:2:end,2:2:end);
  endif
  B=zeros(size(A,1), size(A,2)/2);
  B(1:2:end,:)=Mx;
  B(2:2:end,:)=My;
endfunction

Mz0=unquincunx(M0);
Mz1=unquincunx(M1);
Mz2=unquincunx(M2);

% The 1 frame difference filter is from the article
% A Method of Moving Area-Detection Technique in a MUSE Decoder
h1=[0.5 1 0.5];
h2=[0.5 0 1 0 0.5];
h4=[0.5 0 0 0 1 0 0 0 0.5];
F_line = conv(conv(conv(1,h1),h2),h4);
F1 = [0.5 * F_line; F_line; 0.5 * F_line]; F1 = F1 / sum(sum(F1));

% The 2 frame difference filter was obtained by experiments
F2=fftshift(ifft2(
  [1   1   0   0   0   0   0   0   1;
   0   0   0   0   0   0   0   0   0;
   0   0   0   0   0   0   0   0   0;
   0   0   0   0   0   0   0   0   0]));
F2=F2/sum(sum(F2));

%Fz0=conv2(Mz0, F, "same");
%Fz1=conv2(Mz1, F, "same");
%Fz2=conv2(Mz2, F, "same");

expand = @(A) max([A(2,2) A(1,2)+A(3,2) A(2,1)+A(2,3)]);

Hz0=abs(conv2(Mz0, H, "same"));
edge1=clip(Hz0 / 200, [0, 1]) * 15;
edge2=clip(nlfilter(edge1, [3 3], expand), [0 15]);

% l: level of signal (0-255), e: edge value (0-15)
motion_none = @(l, e) 3 + e / 15 + 0;
motion_full = @(l, e) 5 + e + l / 16;
motion_degree = @(l1, l2, d, e) ...
  clip((d - motion_none((l1+l2)/2, e)) ./ (motion_full((l1+l2)/2, e) - motion_none((l1+l2)/2, e)), ...
       [0, 1]) * 15;

diff1=abs(conv2(Mz0-Mz1, F1, "same"));
diff2=abs(conv2(Mz0-Mz2, F2, "same"));

motion11=motion_degree(Mz0, Mz1, diff1, edge1);
motion12=motion_degree(Mz0, Mz1, diff1, edge2);
motion21=motion_degree(Mz0, Mz2, diff2, edge1);
motion22=motion_degree(Mz0, Mz2, diff2, edge2);

expanded11=clip(nlfilter(motion11, [3 3], expand), [0 15]);
expanded12=clip(nlfilter(motion12, [3 3], expand), [0 15]);
expanded21=clip(nlfilter(motion21, [3 3], expand), [0 15]);
expanded22=clip(nlfilter(motion22, [3 3], expand), [0 15]);

function x=eliminate_single(A)
  B = A;
  B(2,2) = 0;
  m = max(max(B));
  if A(2,2) < m
    x = A(2,2);
  else
    x = m;
  endif
endfunction

eliminated11=clip(nlfilter(motion11, [3 3], @eliminate_single), [0 15]);
eliminated12=clip(nlfilter(motion12, [3 3], @eliminate_single), [0 15]);
eliminated21=clip(nlfilter(motion21, [3 3], @eliminate_single), [0 15]);
eliminated22=clip(nlfilter(motion22, [3 3], @eliminate_single), [0 15]);

expanded_el11=clip(nlfilter(eliminated11, [3 3], expand), [0 15]);
expanded_el12=clip(nlfilter(eliminated12, [3 3], expand), [0 15]);
expanded_el21=clip(nlfilter(eliminated21, [3 3], expand), [0 15]);
expanded_el22=clip(nlfilter(eliminated22, [3 3], expand), [0 15]);

##figure(1);
##subplot(2, 2, 1);
##imshow(Mz0 / 255);
##subplot(2, 2, 2);
##imshow(diff1 / 15);
##subplot(2, 2, 3);
##imshow(expanded11 / 15);
##subplot(2, 2, 4);
##imshow(expanded12 / 15);

figure(1);
subplot(2, 2, 1);
imshow(expanded21 / 15);
subplot(2, 2, 2);
imshow(expanded22 / 15);
subplot(2, 2, 3);
imshow(expanded_el21 / 15);
subplot(2, 2, 4);
imshow(expanded_el22 / 15);

