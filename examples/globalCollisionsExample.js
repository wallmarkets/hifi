//
//  globalCollisionsExample.js
//  hifi
//
//  Created by Brad Hefta-Gaub on 1/29/14.
//  Copyright (c) 2014 HighFidelity, Inc. All rights reserved.
//
//  This is an example script that demonstrates use of the Controller class
//
//


print("hello...");

function particleCollisionWithVoxel(particle, voxel, collision) {
    print("particleCollisionWithVoxel()..");
    print("   particle.getID()=" + particle.id);
    print("   voxel color...=" + voxel.red + ", " + voxel.green + ", " + voxel.blue);
    Vec3.print('penetration=', collision.penetration);
    Vec3.print('contactPoint=', collision.contactPoint);
}

function particleCollisionWithParticle(particleA, particleB, collision) {
    print("particleCollisionWithParticle()..");
    print("   particleA.getID()=" + particleA.id);
    print("   particleB.getID()=" + particleB.id);
    Vec3.print('penetration=', collision.penetration);
    Vec3.print('contactPoint=', collision.contactPoint);
}

Particles.particleCollisionWithVoxel.connect(particleCollisionWithVoxel);
Particles.particleCollisionWithParticle.connect(particleCollisionWithParticle);

print("here... hello...");
