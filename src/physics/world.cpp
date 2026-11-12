#include "physics/world.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace termesh {

namespace {

constexpr float kPenetrationSlop = 0.005f;  // overlap that is tolerated rather than corrected
constexpr float kCorrection = 0.2f;         // fraction of the remaining overlap removed per step
constexpr float kBounceSpeed = 1.0f;        // slower impacts than this do not bounce
constexpr int kIterations = 12;
constexpr float kSleepSpeed = 0.08f, kSleepAngularSpeed = 0.12f, kSleepAfter = 0.6f;
constexpr float kRollingResistance = 1.0f;
constexpr float kWakeSpeed = 0.25f;  // a sleeper touched by something faster than this wakes

// A body's three axes in world space.
std::array<Vec3, 3> axesOf(const Body& body) {
    return {rotate(body.orientation, {1.0f, 0.0f, 0.0f}), rotate(body.orientation, {0.0f, 1.0f, 0.0f}),
            rotate(body.orientation, {0.0f, 0.0f, 1.0f})};
}

float component(Vec3 v, int i) { return i == 0 ? v.x : (i == 1 ? v.y : v.z); }

// The inverse inertia tensor in world space, applied to `v`: rotate into
// the body's frame, where the tensor is diagonal, scale, rotate back.
Vec3 applyInverseInertia(const Body& body, Vec3 v) {
    return rotate(body.orientation, rotate(conjugate(body.orientation), v) * body.inverseInertia);
}

void applyImpulse(Body& body, Vec3 impulse, Vec3 arm) {
    body.velocity += impulse * body.inverseMass;
    body.angularVelocity += applyInverseInertia(body, cross(arm, impulse));
}

// Cut a polygon by a plane, keeping the part where dot(n, p) <= d.
void clipPolygon(std::vector<Vec3>& polygon, Vec3 n, float d) {
    std::vector<Vec3> out;
    const std::size_t count = polygon.size();
    for (std::size_t i = 0; i < count; ++i) {
        const Vec3 p = polygon[i], q = polygon[(i + 1) % count];
        const float dp = dot(n, p) - d, dq = dot(n, q) - d;
        if (dp <= 0.0f) out.push_back(p);
        if ((dp < 0.0f && dq > 0.0f) || (dp > 0.0f && dq < 0.0f)) out.push_back(p + (q - p) * (dp / (dp - dq)));
    }
    polygon.swap(out);
}

}  // namespace

// ------------------------------------------------------------------ bodies ---

int PhysicsWorld::addSphere(Vec3 position, float radius, float density) {
    Body body;
    body.shape = ShapeType::Sphere;
    body.radius = radius;
    body.position = position;
    const float mass = density * (4.0f / 3.0f) * kPi * radius * radius * radius;
    if (mass > 0.0f) {
        body.inverseMass = 1.0f / mass;
        const float inertia = 0.4f * mass * radius * radius;  // a solid sphere: 2/5 m r^2
        body.inverseInertia = {1.0f / inertia, 1.0f / inertia, 1.0f / inertia};
    } else {
        body.inverseMass = 0.0f;
        body.inverseInertia = {0.0f, 0.0f, 0.0f};
    }
    bodies_.push_back(body);
    return static_cast<int>(bodies_.size()) - 1;
}

int PhysicsWorld::addBox(Vec3 position, Vec3 halfExtents, Quat orientation, float density) {
    Body body;
    body.shape = ShapeType::Box;
    body.halfExtents = halfExtents;
    body.position = position;
    body.orientation = normalize(orientation);
    const Vec3 size = halfExtents * 2.0f;
    const float mass = density * size.x * size.y * size.z;
    if (mass > 0.0f) {
        body.inverseMass = 1.0f / mass;
        // A solid box: m (b^2 + c^2) / 12 about the axis along a.
        const Vec3 sq = size * size;
        body.inverseInertia = {12.0f / (mass * (sq.y + sq.z)), 12.0f / (mass * (sq.x + sq.z)),
                               12.0f / (mass * (sq.x + sq.y))};
    } else {
        body.inverseMass = 0.0f;
        body.inverseInertia = {0.0f, 0.0f, 0.0f};
    }
    bodies_.push_back(body);
    return static_cast<int>(bodies_.size()) - 1;
}

bool PhysicsWorld::moving() const {
    return std::any_of(bodies_.begin(), bodies_.end(), [](const Body& b) { return !b.isStatic() && !b.asleep; });
}

float PhysicsWorld::kineticEnergy() const {
    float energy = 0.0f;
    for (const Body& body : bodies_) {
        if (body.isStatic()) continue;
        energy += 0.5f * lengthSquared(body.velocity) / body.inverseMass;
        const Vec3 local = rotate(conjugate(body.orientation), body.angularVelocity);
        const Vec3 inertia{1.0f / body.inverseInertia.x, 1.0f / body.inverseInertia.y, 1.0f / body.inverseInertia.z};
        energy += 0.5f * dot(local * inertia, local);
    }
    return energy;
}

// --------------------------------------------------------------- collision ---

int collideSphereSphere(const Body& a, const Body& b, int ia, int ib, std::vector<Contact>& out) {
    const Vec3 between = b.position - a.position;
    const float distanceSq = lengthSquared(between), reach = a.radius + b.radius;
    if (distanceSq >= reach * reach) return 0;
    const float distance = std::sqrt(distanceSq);
    Contact contact;
    contact.a = ia;
    contact.b = ib;
    contact.normal = distance > 1e-6f ? between / distance : Vec3{0.0f, 1.0f, 0.0f};
    contact.depth = reach - distance;
    // Half-way through the overlap.
    contact.point = a.position + contact.normal * (a.radius - contact.depth * 0.5f);
    out.push_back(contact);
    return 1;
}

int collideSphereBox(const Body& sphere, const Body& box, int isphere, int ibox, std::vector<Contact>& out) {
    // Work in the box's own frame, where it is axis-aligned: the closest
    // point of the box to the sphere's centre is the centre clamped to it.
    const Vec3 local = rotate(conjugate(box.orientation), sphere.position - box.position);
    const Vec3 h = box.halfExtents;
    const Vec3 closest{std::clamp(local.x, -h.x, h.x), std::clamp(local.y, -h.y, h.y), std::clamp(local.z, -h.z, h.z)};
    const Vec3 offset = local - closest;
    const float distanceSq = lengthSquared(offset);
    if (distanceSq >= sphere.radius * sphere.radius) return 0;

    Contact contact;
    contact.a = isphere;
    contact.b = ibox;
    Vec3 normalLocal;  // from the sphere towards the box
    if (distanceSq > 1e-10f) {
        const float distance = std::sqrt(distanceSq);
        normalLocal = offset / -distance;
        contact.depth = sphere.radius - distance;
    } else {
        // The centre is inside the box: leave by the nearest face.
        const float gaps[3] = {h.x - std::abs(local.x), h.y - std::abs(local.y), h.z - std::abs(local.z)};
        const int axis = gaps[0] < gaps[1] ? (gaps[0] < gaps[2] ? 0 : 2) : (gaps[1] < gaps[2] ? 1 : 2);
        normalLocal = {0.0f, 0.0f, 0.0f};
        const float side = component(local, axis) >= 0.0f ? 1.0f : -1.0f;
        (axis == 0 ? normalLocal.x : axis == 1 ? normalLocal.y : normalLocal.z) = -side;
        contact.depth = sphere.radius + gaps[axis];
    }
    contact.normal = rotate(box.orientation, normalLocal);
    contact.point = box.position + rotate(box.orientation, closest);
    out.push_back(contact);
    return 1;
}

int collideGround(const Body& body, int index, float height, float extent, std::vector<Contact>& out) {
    const Vec3 down{0.0f, -1.0f, 0.0f};
    // Whether a point is over the floor rather than past its edge - or so
    // far below it that it has already fallen past.
    const auto over = [&](Vec3 p) {
        return (extent <= 0.0f || (std::abs(p.x) <= extent && std::abs(p.z) <= extent)) && p.y > height - 1.0f;
    };
    if (body.shape == ShapeType::Sphere) {
        const float depth = height - (body.position.y - body.radius);
        if (depth <= 0.0f || !over(body.position)) return 0;
        out.push_back({index, -1, {body.position.x, height, body.position.z}, down, depth});
        return 1;
    }
    // A box touches the ground with its corners.
    const auto axes = axesOf(body);
    int count = 0;
    for (int corner = 0; corner < 8; ++corner) {
        const Vec3 p = body.position + axes[0] * ((corner & 1) ? body.halfExtents.x : -body.halfExtents.x) +
                       axes[1] * ((corner & 2) ? body.halfExtents.y : -body.halfExtents.y) +
                       axes[2] * ((corner & 4) ? body.halfExtents.z : -body.halfExtents.z);
        if (p.y >= height || !over(p)) continue;
        out.push_back({index, -1, p, down, height - p.y});
        ++count;
    }
    return count;
}

// Two boxes, by the separating axis theorem: two convex shapes are apart
// exactly when there is some axis along which their shadows do not overlap,
// and for boxes only fifteen axes need trying - the three face normals of
// each, and the nine cross products of an edge of one with an edge of the
// other. If none separates them, the axis with the least overlap says which
// way to push, and what kind of contact it is: face against face (or
// corner, or edge), or edge across edge.
int collideBoxBox(const Body& a, const Body& b, int ia, int ib, std::vector<Contact>& out) {
    const auto axesA = axesOf(a), axesB = axesOf(b);
    const Vec3 between = b.position - a.position;

    // Half the length of a box's shadow on an axis.
    const auto reach = [](const Body& body, const std::array<Vec3, 3>& axes, Vec3 axis) {
        return body.halfExtents.x * std::abs(dot(axes[0], axis)) + body.halfExtents.y * std::abs(dot(axes[1], axis)) +
               body.halfExtents.z * std::abs(dot(axes[2], axis));
    };

    float bestOverlap = 1e30f;
    Vec3 bestAxis;
    int bestKind = -1;  // 0-2: face of a, 3-5: face of b, 6-14: edge pair
    const auto test = [&](Vec3 axis, int kind) {
        const float lengthSq = lengthSquared(axis);
        if (lengthSq < 1e-8f) return true;  // parallel edges: no new axis
        axis = axis / std::sqrt(lengthSq);
        const float overlap = reach(a, axesA, axis) + reach(b, axesB, axis) - std::abs(dot(between, axis));
        if (overlap <= 0.0f) return false;
        // Prefer a face over an edge pair unless the edges are clearly
        // better: face contacts have more points and rest more steadily.
        const float biased = kind >= 6 ? overlap * 1.05f + 0.002f : overlap;
        if (biased < bestOverlap) {
            bestOverlap = biased;
            bestAxis = axis;
            bestKind = kind;
        }
        return true;
    };
    for (int i = 0; i < 3; ++i)
        if (!test(axesA[static_cast<std::size_t>(i)], i)) return 0;
    for (int i = 0; i < 3; ++i)
        if (!test(axesB[static_cast<std::size_t>(i)], 3 + i)) return 0;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            if (!test(cross(axesA[static_cast<std::size_t>(i)], axesB[static_cast<std::size_t>(j)]), 6 + i * 3 + j))
                return 0;

    // From a towards b.
    Vec3 normal = dot(bestAxis, between) < 0.0f ? -bestAxis : bestAxis;
    const float overlap = reach(a, axesA, normal) + reach(b, axesB, normal) - std::abs(dot(between, normal));

    if (bestKind >= 6) {
        // Edge across edge: one contact, between the closest points of the
        // two edges. On each box, the edge is the one furthest towards the
        // other box.
        const int i = (bestKind - 6) / 3, j = (bestKind - 6) % 3;
        Vec3 pointA = a.position, pointB = b.position;
        for (int k = 0; k < 3; ++k) {
            const auto ks = static_cast<std::size_t>(k);
            if (k != i)
                pointA += axesA[ks] * (component(a.halfExtents, k) * (dot(axesA[ks], normal) > 0.0f ? 1.0f : -1.0f));
            if (k != j)
                pointB += axesB[ks] * (component(b.halfExtents, k) * (dot(axesB[ks], normal) > 0.0f ? -1.0f : 1.0f));
        }
        const Vec3 da = axesA[static_cast<std::size_t>(i)], db = axesB[static_cast<std::size_t>(j)];
        const Vec3 r = pointA - pointB;
        const float ab = dot(da, db), denominator = 1.0f - ab * ab;
        float s = 0.0f, t = 0.0f;
        if (denominator > 1e-6f) {
            s = (ab * dot(db, r) - dot(da, r)) / denominator;
            t = (dot(db, r) - ab * dot(da, r)) / denominator;
        }
        s = std::clamp(s, -component(a.halfExtents, i), component(a.halfExtents, i));
        t = std::clamp(t, -component(b.halfExtents, j), component(b.halfExtents, j));
        out.push_back({ia, ib, (pointA + da * s + pointB + db * t) * 0.5f, normal, overlap});
        return 1;
    }

    // Face contact. The "reference" box owns the face; the other box's face
    // most nearly opposite it (the "incident" face) is clipped to the
    // reference face's outline, and whatever is left below the reference
    // face is where they touch.
    const bool referenceIsA = bestKind < 3;
    const Body& reference = referenceIsA ? a : b;
    const Body& incident = referenceIsA ? b : a;
    const auto& referenceAxes = referenceIsA ? axesA : axesB;
    const auto& incidentAxes = referenceIsA ? axesB : axesA;
    const Vec3 outward = referenceIsA ? normal : -normal;  // out of the reference face
    const int faceAxis = referenceIsA ? bestKind : bestKind - 3;

    // The incident face: the one whose normal points most against `outward`.
    int incidentAxis = 0;
    float most = -1.0f;
    for (int k = 0; k < 3; ++k) {
        const float alignment = std::abs(dot(incidentAxes[static_cast<std::size_t>(k)], outward));
        if (alignment > most) most = alignment, incidentAxis = k;
    }
    const Vec3 incidentNormal =
        incidentAxes[static_cast<std::size_t>(incidentAxis)] *
        (dot(incidentAxes[static_cast<std::size_t>(incidentAxis)], outward) > 0.0f ? -1.0f : 1.0f);
    const int u = (incidentAxis + 1) % 3, v = (incidentAxis + 2) % 3;
    const Vec3 centre = incident.position + incidentNormal * component(incident.halfExtents, incidentAxis);
    const Vec3 eu = incidentAxes[static_cast<std::size_t>(u)] * component(incident.halfExtents, u);
    const Vec3 ev = incidentAxes[static_cast<std::size_t>(v)] * component(incident.halfExtents, v);
    std::vector<Vec3> polygon{centre + eu + ev, centre - eu + ev, centre - eu - ev, centre + eu - ev};

    // Clip to the four sides of the reference face.
    for (int k = 0; k < 3 && !polygon.empty(); ++k) {
        if (k == faceAxis) continue;
        const Vec3 side = referenceAxes[static_cast<std::size_t>(k)];
        const float extent = component(reference.halfExtents, k), offset = dot(side, reference.position);
        clipPolygon(polygon, side, offset + extent);
        clipPolygon(polygon, -side, -offset + extent);
    }

    const float faceOffset = dot(outward, reference.position) + component(reference.halfExtents, faceAxis);
    int count = 0;
    for (const Vec3& p : polygon) {
        const float depth = faceOffset - dot(outward, p);
        if (depth < 0.0f) continue;
        out.push_back({ia, ib, p + outward * (depth * 0.5f), normal, depth});
        ++count;
    }
    return count;
}

void PhysicsWorld::findContacts() {
    contacts_.clear();
    const int count = static_cast<int>(bodies_.size());
    for (int i = 0; i < count; ++i) {
        const Body& a = bodies_[static_cast<std::size_t>(i)];
        if (hasGround && !a.isStatic() && a.position.y - a.boundingRadius() < groundHeight)
            collideGround(a, i, groundHeight, groundExtent, contacts_);
        for (int j = i + 1; j < count; ++j) {
            const Body& b = bodies_[static_cast<std::size_t>(j)];
            // Nothing to do for two bodies that cannot move, or are both
            // at rest - or that are too far apart to touch.
            if ((a.isStatic() || a.asleep) && (b.isStatic() || b.asleep)) continue;
            const float reach = a.boundingRadius() + b.boundingRadius();
            if (lengthSquared(b.position - a.position) >= reach * reach) continue;

            if (a.shape == ShapeType::Sphere && b.shape == ShapeType::Sphere)
                collideSphereSphere(a, b, i, j, contacts_);
            else if (a.shape == ShapeType::Sphere) collideSphereBox(a, b, i, j, contacts_);
            else if (b.shape == ShapeType::Sphere) collideSphereBox(b, a, j, i, contacts_);
            else collideBoxBox(a, b, i, j, contacts_);
        }
    }
}

// ------------------------------------------------------------------ solver ---
//
// Sequential impulses. Each contact is a small, local rule: "these two
// points must not be approaching each other along the normal". Fixing one
// contact disturbs its neighbours, so the contacts are visited over and
// over, each time applying just the impulse that satisfies that one; the
// corrections shrink with every pass and the whole pile settles on
// velocities that satisfy all of them at once.

void PhysicsWorld::solve() {
    struct Row {
        Vec3 armA, armB;
        Vec3 normal, tangent1, tangent2;
        float normalMass = 0.0f, tangentMass1 = 0.0f, tangentMass2 = 0.0f;
        float bias = 0.0f;  // the separating speed the contact should end up with
        float friction = 0.0f;
        float normalImpulse = 0.0f, tangentImpulse1 = 0.0f, tangentImpulse2 = 0.0f;  // totals so far
    };
    std::vector<Row> rows(contacts_.size());
    // The ground takes part as a body that nothing can move.
    Body ground;
    ground.inverseMass = 0.0f;
    ground.inverseInertia = {0.0f, 0.0f, 0.0f};
    const auto bodyB = [&](const Contact& contact) -> Body& {
        return contact.b >= 0 ? bodies_[static_cast<std::size_t>(contact.b)] : ground;
    };

    // How hard it is to change the relative speed of the two contact points
    // along `direction`: the "effective mass" seen by an impulse there.
    const auto effectiveMass = [](const Body& a, const Body& b, Vec3 armA, Vec3 armB, Vec3 direction) {
        const Vec3 ra = cross(armA, direction), rb = cross(armB, direction);
        const float k =
            a.inverseMass + b.inverseMass + dot(ra, applyInverseInertia(a, ra)) + dot(rb, applyInverseInertia(b, rb));
        return k > 0.0f ? 1.0f / k : 0.0f;
    };

    for (std::size_t i = 0; i < contacts_.size(); ++i) {
        const Contact& contact = contacts_[i];
        Body& a = bodies_[static_cast<std::size_t>(contact.a)];
        Body& b = bodyB(contact);
        Row& row = rows[i];
        row.armA = contact.point - a.position;
        row.armB = contact.b >= 0 ? contact.point - b.position : Vec3{};
        row.normal = contact.normal;
        // Any two directions at right angles to the normal, for friction.
        const Vec3 helper = std::abs(row.normal.x) < 0.57f ? Vec3{1.0f, 0.0f, 0.0f} : Vec3{0.0f, 1.0f, 0.0f};
        row.tangent1 = normalize(cross(row.normal, helper));
        row.tangent2 = cross(row.normal, row.tangent1);
        row.normalMass = effectiveMass(a, b, row.armA, row.armB, row.normal);
        row.tangentMass1 = effectiveMass(a, b, row.armA, row.armB, row.tangent1);
        row.tangentMass2 = effectiveMass(a, b, row.armA, row.armB, row.tangent2);

        const float restitution = std::max(a.restitution, contact.b >= 0 ? b.restitution : groundRestitution);
        row.friction = contact.b >= 0 ? std::sqrt(a.friction * b.friction) : std::sqrt(a.friction * groundFriction);

        // Push the overlap apart gently, a fraction per step, and bounce if
        // the impact is fast enough to notice.
        row.bias = kCorrection / kTimeStep * std::max(contact.depth - kPenetrationSlop, 0.0f);
        const float approach = dot(row.normal, b.velocity + cross(b.angularVelocity, row.armB) - a.velocity -
                                                   cross(a.angularVelocity, row.armA));
        if (approach < -kBounceSpeed) row.bias = std::max(row.bias, -restitution * approach);

        // Being touched by something that is really moving wakes a sleeper.
        // (Merely being awake is not enough: two bodies resting on each
        // other would keep waking one another for ever.)
        const auto disturbs = [](const Body& body) {
            return !body.asleep && !body.isStatic() &&
                   (lengthSquared(body.velocity) > kWakeSpeed * kWakeSpeed ||
                    lengthSquared(body.angularVelocity) > kWakeSpeed * kWakeSpeed);
        };
        if (contact.b >= 0) {
            if (a.asleep && disturbs(b)) a.asleep = false, a.quietTime = 0.0f;
            if (b.asleep && disturbs(a)) b.asleep = false, b.quietTime = 0.0f;
        }
    }

    for (int iteration = 0; iteration < kIterations; ++iteration) {
        for (std::size_t i = 0; i < contacts_.size(); ++i) {
            const Contact& contact = contacts_[i];
            Body& a = bodies_[static_cast<std::size_t>(contact.a)];
            Body& b = bodyB(contact);
            Row& row = rows[i];
            if ((a.asleep || a.isStatic()) && (b.asleep || b.isStatic())) continue;

            const auto relativeVelocity = [&] {
                return b.velocity + cross(b.angularVelocity, row.armB) - a.velocity -
                       cross(a.angularVelocity, row.armA);
            };
            const auto apply = [&](Vec3 impulse) {
                if (!a.asleep) applyImpulse(a, -impulse, row.armA);
                if (!b.asleep) applyImpulse(b, impulse, row.armB);
            };

            // Along the normal: contacts push, they never pull. It is the
            // running total that must not go negative, not each correction
            // - a later pass may take back some of what an earlier one gave.
            {
                const float change = (row.bias - dot(relativeVelocity(), row.normal)) * row.normalMass;
                const float before = row.normalImpulse;
                row.normalImpulse = std::max(before + change, 0.0f);
                apply(row.normal * (row.normalImpulse - before));
            }
            // Friction: oppose sliding, but with no more force than the
            // contact's pressure allows (Coulomb's law).
            const float limit = row.friction * row.normalImpulse;
            {
                const float change = -dot(relativeVelocity(), row.tangent1) * row.tangentMass1;
                const float before = row.tangentImpulse1;
                row.tangentImpulse1 = std::clamp(before + change, -limit, limit);
                apply(row.tangent1 * (row.tangentImpulse1 - before));
            }
            {
                const float change = -dot(relativeVelocity(), row.tangent2) * row.tangentMass2;
                const float before = row.tangentImpulse2;
                row.tangentImpulse2 = std::clamp(before + change, -limit, limit);
                apply(row.tangent2 * (row.tangentImpulse2 - before));
            }
        }
    }
}

void PhysicsWorld::integrate() {
    for (Body& body : bodies_) {
        if (body.isStatic() || body.asleep) continue;
        body.position += body.velocity * kTimeStep;
        // Turn by the angular velocity: q' = q + 1/2 w q dt.
        const Vec3 w = body.angularVelocity;
        const Quat spin = Quat{w.x, w.y, w.z, 0.0f} * body.orientation;
        body.orientation = normalize(
            Quat{body.orientation.x + 0.5f * kTimeStep * spin.x, body.orientation.y + 0.5f * kTimeStep * spin.y,
                 body.orientation.z + 0.5f * kTimeStep * spin.z, body.orientation.w + 0.5f * kTimeStep * spin.w});
    }
}

void PhysicsWorld::updateSleep() {
    for (Body& body : bodies_) {
        if (body.isStatic() || body.asleep) continue;
        const bool quiet = lengthSquared(body.velocity) < kSleepSpeed * kSleepSpeed &&
                           lengthSquared(body.angularVelocity) < kSleepAngularSpeed * kSleepAngularSpeed;
        body.quietTime = quiet ? body.quietTime + kTimeStep : 0.0f;
        if (body.quietTime >= kSleepAfter) {
            body.asleep = true;
            body.velocity = {};
            body.angularVelocity = {};
        }
    }
}

void PhysicsWorld::step() {
    for (Body& body : bodies_) {
        if (body.isStatic() || body.asleep) continue;
        body.velocity += gravity * kTimeStep;
        // A little drag, standing in for the air: without it a spinning
        // body would spin for ever.
        body.velocity *= 1.0f / (1.0f + kTimeStep * 0.02f);
        body.angularVelocity *= 1.0f / (1.0f + kTimeStep * 0.25f);
    }
    findContacts();
    solve();
    // Rolling resistance: a real ball on a real floor does not roll for
    // ever. Friction alone cannot stop it - a rolling ball's contact point
    // is not sliding - so balls that are touching something lose a little
    // spin each step.
    for (const Contact& contact : contacts_) {
        for (int index : {contact.a, contact.b}) {
            if (index < 0) continue;
            Body& body = bodies_[static_cast<std::size_t>(index)];
            if (body.shape == ShapeType::Sphere && !body.asleep)
                body.angularVelocity *= 1.0f / (1.0f + kTimeStep * kRollingResistance);
        }
    }
    integrate();
    updateSleep();
}

void PhysicsWorld::advance(float dt) {
    // Never try to catch up on more than a few steps: if the program was
    // stopped for a second, the world should not lurch forward by one.
    accumulator_ = std::min(accumulator_ + std::max(dt, 0.0f), kTimeStep * 8.0f);
    while (accumulator_ >= kTimeStep) {
        step();
        accumulator_ -= kTimeStep;
    }
}

}  // namespace termesh
