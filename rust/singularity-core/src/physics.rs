//! A fixed-step physics integrator for entity overrides.
//!
//! The engine does not replace the game's collision; it *overrides* the acceleration an entity
//! experiences (gravity, wind, custom forces) and integrates the velocity, leaving collision to the
//! game's own swept tests. That split is deliberate: the game's collision is tuned for its AABB
//! world, and re-implementing it would desync the client from the server.
//!
//! The integrator is a pure function of `(state, forces, dt)`, so a pack's custom gravity is
//! reproducible and testable without a device.

/// A vector in world space.
#[derive(Debug, Clone, Copy, PartialEq, Default)]
pub struct Vec3 {
    pub x: f32,
    pub y: f32,
    pub z: f32,
}

impl Vec3 {
    pub const ZERO: Vec3 = Vec3 {
        x: 0.0,
        y: 0.0,
        z: 0.0,
    };

    pub fn new(x: f32, y: f32, z: f32) -> Vec3 {
        Vec3 { x, y, z }
    }

    pub fn add(self, other: Vec3) -> Vec3 {
        Vec3::new(self.x + other.x, self.y + other.y, self.z + other.z)
    }

    pub fn scale(self, factor: f32) -> Vec3 {
        Vec3::new(self.x * factor, self.y * factor, self.z * factor)
    }

    pub fn length(self) -> f32 {
        (self.x * self.x + self.y * self.y + self.z * self.z).sqrt()
    }

    /// The vector clamped to a maximum length.
    pub fn clamp_length(self, max: f32) -> Vec3 {
        let length = self.length();
        if length > max && length > 0.0 {
            self.scale(max / length)
        } else {
            self
        }
    }
}

/// An entity's physics state.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Body {
    pub position: Vec3,
    pub velocity: Vec3,
    /// A per-entity multiplier on the global gravity (1 is normal, 0.16 is roughly the moon).
    pub gravity_scale: f32,
    /// Air resistance per step, in `[0, 1)`; 0 means no drag.
    pub drag: f32,
    pub on_ground: bool,
}

impl Default for Body {
    fn default() -> Self {
        Body {
            position: Vec3::ZERO,
            velocity: Vec3::ZERO,
            gravity_scale: 1.0,
            drag: 0.0,
            on_ground: false,
        }
    }
}

/// The global physics environment, from the `.singularity` physics pack.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Environment {
    pub gravity: Vec3,
    /// A wind velocity applied to airborne entities, scaled by altitude.
    pub wind: Vec3,
    /// How much stronger the wind gets per block above the base altitude.
    pub wind_altitude_gain: f32,
    /// The altitude at which wind begins to strengthen.
    pub wind_base_altitude: f32,
    /// The fastest an entity may move, so a misconfigured force cannot fling it across the world.
    pub max_speed: f32,
}

impl Default for Environment {
    fn default() -> Self {
        Environment {
            gravity: Vec3::new(0.0, -9.8, 0.0),
            wind: Vec3::ZERO,
            wind_altitude_gain: 0.0,
            wind_base_altitude: 64.0,
            max_speed: 100.0,
        }
    }
}

/// A physics material for a block or surface.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Material {
    /// The fraction of horizontal speed *lost* when an entity lands. 0 is frictionless (ice: all
    /// speed is kept); 1 is fully grippy (the landing kills the horizontal motion).
    pub friction: f32,
    /// 0 is a normal surface; 1 is a perfect trampoline.
    pub restitution: f32,
}

impl Material {
    pub const NORMAL: Material = Material {
        friction: 0.6,
        restitution: 0.0,
    };

    pub fn new(friction: f32, restitution: f32) -> Material {
        Material {
            friction: friction.clamp(0.0, 1.0),
            restitution: restitution.clamp(0.0, 1.0),
        }
    }
}

/// The fixed timestep the integrator uses, in seconds (60 Hz).
pub const FIXED_STEP: f32 = 1.0 / 60.0;

/// Advances one body by `dt`, applying gravity, wind and drag.
///
/// `extra_forces` are the per-entity forces a mod added this step. The result is the new state;
/// collision is the caller's job.
pub fn integrate(body: &Body, environment: &Environment, extra_forces: &[Vec3], dt: f32) -> Body {
    let mut acceleration = environment.gravity.scale(body.gravity_scale);

    // Wind strengthens with altitude.
    if environment.wind.length() > 0.0 {
        let above = (body.position.y - environment.wind_base_altitude).max(0.0);
        let gain = 1.0 + above * environment.wind_altitude_gain;
        acceleration = acceleration.add(environment.wind.scale(gain));
    }

    for force in extra_forces {
        acceleration = acceleration.add(*force);
    }

    let mut velocity = body.velocity.add(acceleration.scale(dt));
    // Drag is a per-step multiplicative decay.
    if body.drag > 0.0 {
        velocity = velocity.scale(1.0 - body.drag.clamp(0.0, 0.99));
    }
    velocity = velocity.clamp_length(environment.max_speed);
    let position = body.position.add(velocity.scale(dt));

    Body {
        position,
        velocity,
        ..*body
    }
}

/// Resolves a landing on a surface with a material: friction damps horizontal velocity, restitution
/// reflects vertical velocity.
pub fn resolve_landing(body: &Body, material: Material) -> Body {
    let mut velocity = body.velocity;
    let keep = 1.0 - material.friction.clamp(0.0, 1.0);
    velocity.x *= keep;
    velocity.z *= keep;
    if velocity.y < 0.0 {
        velocity.y = -velocity.y * material.restitution;
        // Stop a tiny bounce from jittering forever.
        if velocity.y.abs() < 0.05 {
            velocity.y = 0.0;
        }
    }
    Body {
        velocity,
        on_ground: true,
        ..*body
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn gravity_pulls_a_body_down() {
        let body = Body {
            position: Vec3::new(0.0, 100.0, 0.0),
            ..Body::default()
        };
        let stepped = integrate(&body, &Environment::default(), &[], FIXED_STEP);
        assert!(stepped.velocity.y < 0.0);
        assert!(stepped.position.y < 100.0);
    }

    #[test]
    fn a_lower_gravity_scale_falls_slower() {
        let normal = Body::default();
        let moon = Body {
            gravity_scale: 0.16,
            ..Body::default()
        };
        let env = Environment::default();
        let normal_step = integrate(&normal, &env, &[], FIXED_STEP);
        let moon_step = integrate(&moon, &env, &[], FIXED_STEP);
        assert!(moon_step.velocity.y > normal_step.velocity.y, "moon gravity should fall slower");
    }

    #[test]
    fn wind_strengthens_with_altitude() {
        let env = Environment {
            gravity: Vec3::ZERO,
            wind: Vec3::new(1.0, 0.0, 0.0),
            wind_altitude_gain: 0.01,
            wind_base_altitude: 64.0,
            max_speed: 100.0,
        };
        let low = Body {
            position: Vec3::new(0.0, 64.0, 0.0),
            ..Body::default()
        };
        let high = Body {
            position: Vec3::new(0.0, 164.0, 0.0),
            ..Body::default()
        };
        let low_step = integrate(&low, &env, &[], FIXED_STEP);
        let high_step = integrate(&high, &env, &[], FIXED_STEP);
        assert!(high_step.velocity.x > low_step.velocity.x, "wind should be stronger higher up");
    }

    #[test]
    fn speed_is_capped() {
        let env = Environment {
            max_speed: 10.0,
            ..Environment::default()
        };
        let body = Body {
            velocity: Vec3::new(1000.0, 0.0, 0.0),
            ..Body::default()
        };
        let stepped = integrate(&body, &env, &[], FIXED_STEP);
        assert!(stepped.velocity.length() <= 10.0 + 1e-3);
    }

    #[test]
    fn extra_forces_accumulate() {
        let env = Environment {
            gravity: Vec3::ZERO,
            max_speed: 1000.0,
            ..Environment::default()
        };
        let body = Body::default();
        let forces = [Vec3::new(1.0, 0.0, 0.0), Vec3::new(1.0, 0.0, 0.0)];
        let stepped = integrate(&body, &env, &forces, FIXED_STEP);
        assert!(stepped.velocity.x > 0.0);
    }

    #[test]
    fn a_bouncy_material_reflects_the_landing() {
        let body = Body {
            velocity: Vec3::new(1.0, -5.0, 0.0),
            ..Body::default()
        };
        let landed = resolve_landing(&body, Material::new(0.0, 0.9));
        assert!(landed.velocity.y > 0.0, "a bouncy surface should reverse the fall");
        assert!(landed.on_ground);
    }

    #[test]
    fn an_icy_material_keeps_horizontal_speed() {
        let body = Body {
            velocity: Vec3::new(5.0, 0.0, 5.0),
            ..Body::default()
        };
        // `friction` is the fraction of speed lost: 0 keeps it all (ice), 1 kills it (grippy).
        let icy = resolve_landing(&body, Material::new(0.0, 0.0));
        let grippy = resolve_landing(&body, Material::new(0.9, 0.0));
        assert!(icy.velocity.x > grippy.velocity.x);
    }

    #[test]
    fn a_tiny_bounce_settles() {
        let body = Body {
            velocity: Vec3::new(0.0, -0.01, 0.0),
            ..Body::default()
        };
        let landed = resolve_landing(&body, Material::new(0.0, 0.1));
        assert_eq!(landed.velocity.y, 0.0);
    }
}
