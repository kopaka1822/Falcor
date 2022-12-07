# helper file to generate points on a poisson disc
import random
import math

def pow2(x):
    return x * x

# number of samples
n = 16
# radius of poisson discs
r = 0.19
#r = 0.28 # good radius if sample is completely inside the circle (1-r)
#r = 0.13
#r = 0.38
min_distance = 2 * r

max_retries = 10000

# generate n samples on a uniform disc with poisson disc sampling
sample_points = []
cur_attempt = 1
while(len(sample_points) < n):
    print("attempt ", cur_attempt)
    sample_points = []
    cur_retries = 0
    cur_attempt += 1

    while(len(sample_points) < n and cur_retries < max_retries):
        cur_retries += 1
        # generate a random point on the disc
        x = random.uniform(-1, 1)
        y = random.uniform(-1, 1)
        # reject point if it is not in the unit disc
        if x*x + y*y > pow2(1 - r):
        #if x*x + y*y > pow2(1):
            continue

        # reject point if it is too close to another point
        too_close = False
        for point in sample_points:
            if pow2(x - point[0]) + pow2(y - point[1]) < pow2(min_distance):
                too_close = True
                break

        if too_close:
            continue

        # add point to list
        sample_points.append((x, y))

# plot sample_points as points
import matplotlib.pyplot as plt
# draw unit disc
fig, ax = plt.subplots()
ax.add_patch(plt.Circle((0, 0), 1, color='r', fill=False))
# draw circle with radius r around each sample point
for point in sample_points:
    ax.add_patch(plt.Circle(point, r, color='b', fill=False))

ax.add_patch(plt.Circle(sample_points[0], r, color='g', fill=False))

plt.plot(*zip(*sample_points), marker='o', linestyle='None')

print("x,y pairs:")
# print all points in sample_points
for point in sample_points:
    # print point[0] and point[1] with 3 decimals
    print("({0:.3f}, {1:.3f})".format(point[0], point[1]), end=",")
print("")
plt.show()
