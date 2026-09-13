# Use Cases — MongoDB

MongoDB's document model and distributed architecture make it a strong fit for a broad range of applications, and a poor fit for others. This document covers where it shines, where it does not, and a concrete example scenario.

---

## Ideal use cases

### Content management systems

Blogs, news sites, CMS platforms, and documentation stores benefit from flexible documents that mix structured metadata (author, tags, publish date) with free-form rich text bodies. Different content types can live in one collection without a rigid schema.

```js
db.articles.insertOne({
  _id: ObjectId(),
  slug: "mongodb-sharding",
  title: "Understanding Sharding",
  author: { id: 1, name: "Ada" },
  tags: ["mongodb", "scaling"],
  body: "Long rich-text content...",
  publishedAt: ISODate("2026-02-01"),
  media: [ "cover.png", "diagram.svg" ]
})
```

### Product catalogs

E-commerce and inventory catalogs have products with widely varying attributes (books have authors/ISBN; electronics have specs; apparel has sizes/colors). A document model handles this heterogeneity naturally without sparse columns or EAV tables. See the example scenario below.

### Real-time analytics and event ingestion

High-throughput event streams (clickstreams, IoT telemetry, logs) can be written fast and aggregated with the pipeline. Time-series collections (introduced in 5.0) are optimized for this.

```js
db.sensorReadings.insertOne({
  sensorId: "s-42",
  ts: ISODate("2026-02-01T10:00:00Z"),
  tempC: 21.4,
  humidity: 55
})
```

### Mobile and IoT application backends

Mobile apps and IoT devices produce semi-structured data that evolves across app versions. MongoDB's schema flexibility means a new app version can add fields without migrating old records. Offline-first sync (via Realm/Atlas Device Sync) integrates well.

### Caching and metadata stores

Because documents are keyed by `_id` and lookups are O(1) on the index, MongoDB works as a metadata or session store. TTL indexes auto-expire stale entries (e.g., sessions, OTP codes).

```js
db.sessions.createIndex({ createdAt: 1 }, { expireAfterSeconds: 1800 })
db.sessions.insertOne({ _id: "tok-abc", userId: 7, createdAt: new Date() })
```

### Rapidly evolving schemas

Startups and experimental products change their data model frequently. Adding a field requires no migration; embedding avoids join-table churn. Teams can ship faster than with a normalized RDBMS.

### Personalization and user profiles

User preferences, activity history, and recommendations fit a nested document. Reads are typically by user id (a single-document fetch), which is fast and scalable.

### Geospatial and location services

Native `2dsphere` indexes and geo queries make MongoDB suitable for location-aware apps (store finders, delivery tracking, geofencing).

---

## When NOT to use MongoDB

### Complex multi-entity transactions

If your core workload is many interdependent writes across entities that must all succeed or fail together (e.g., double-entry accounting, booking systems with strict inventory holds), a relational database with mature transaction handling is usually safer and faster. MongoDB transactions exist but carry overhead and are not ideal for high-contention hot paths.

### Heavy relational joins and reporting

Business intelligence, regulatory reporting, and analytics that join many tables, enforce foreign-key constraints, and run complex `GROUP BY`s are better served by a columnar/relational warehouse (PostgreSQL, Snowflake, BigQuery). `$lookup` chains become unwieldy and slow at scale.

### Rigid schema / regulatory relational needs

Industries requiring strict relational integrity, audit trails via constraints, and normalized, validated relational models (core banking, ERP, healthcare claims with rigid schemas) often prefer an RDBMS where constraints are enforced by the engine, not by application discipline.

### Ad-hoc arbitrary reporting on the same store

Running heavy analytical/reporting queries on the same MongoDB instance that serves production traffic can starve the operational workload. The common pattern is to replicate to a secondary or stream to a dedicated analytics store.

---

## Example scenario: E-commerce product catalog

Consider an online store with products that have very different attributes. We model each product as a single document with embedded variants and a nested spec object, and use an aggregation pipeline to compute category-level statistics.

### Document model

```js
db.products.insertOne({
  _id: ObjectId("64f1c2..."),
  sku: "ELEC-MOUSE-01",
  name: "Wireless Mouse",
  category: "electronics",
  brand: "Acme",
  price: NumberDecimal("29.99"),
  currency: "USD",
  inStock: true,
  tags: ["wireless", "usb-c", "ergonomic"],
  attributes: {
    connectivity: "Bluetooth 5.0",
    dpi: 1600,
    batteryLifeHrs: 120
  },
  variants: [
    { color: "Black",  stock: 50 },
    { color: "White",  stock: 30 }
  ],
  ratings: { avg: 4.6, count: 128 },
  createdAt: ISODate("2026-01-15")
})
```

A book would simply have different fields:

```js
db.products.insertOne({
  _id: ObjectId("64f1c3..."),
  sku: "BOOK-DB-01",
  name: "Designing Data-Intensive Applications",
  category: "books",
  author: "Martin Kleppmann",
  isbn: "978-1449373320",
  price: NumberDecimal("39.99"),
  currency: "USD",
  inStock: true,
  attributes: { pages: 616, language: "English" },
  ratings: { avg: 4.8, count: 9000 }
})
```

### Indexing for common queries

```js
// Category + price range queries
db.products.createIndex({ category: 1, price: 1 })

// Text search on name
db.products.createIndex({ name: "text" })

// Fast lookup by SKU
db.products.createIndex({ sku: 1 }, { unique: true })
```

### Aggregation sketch: best-selling categories

Compute total inventory value and average rating per category:

```js
db.products.aggregate([
  // Only in-stock products
  { $match: { inStock: true } },

  // Compute per-document inventory value and total stock
  { $addFields: {
      totalStock: { $sum: "$variants.stock" }
  }},

  // Group by category
  { $group: {
      _id: "$category",
      productCount:   { $sum: 1 },
      inventoryValue: { $sum: { $multiply: ["$price", { $ifNull: ["$totalStock", 0] }] } },
      avgRating:      { $avg: "$ratings.avg" }
  }},

  // Sort by inventory value descending
  { $sort: { inventoryValue: -1 } },

  // Round numbers for presentation
  { $project: {
      category: "$_id",
      productCount: 1,
      inventoryValue: { $round: ["$inventoryValue", 2] },
      avgRating: { $round: ["$avgRating", 2] }
  }}
])
```

### Query sketch: faceted search

Find electronics under $50 with a text match, projecting only needed fields:

```js
db.products.find(
  { category: "electronics", price: { $lt: NumberDecimal("50.00") }, $text: { $search: "wireless" } },
  { name: 1, price: 1, ratings: 1, _id: 0 }
).sort({ "ratings.avg": -1 }).limit(20)
```

### Why this fits MongoDB

- Products have heterogeneous attributes → no schema migration when adding a new product type.
- Reads are mostly by category / sku / text → served by indexes.
- Inventory value and ratings are computed on demand via the pipeline, avoiding pre-aggregation maintenance.
- If the store grows, the `products` collection can be sharded on `category` or `sku` to scale out.

### When it would NOT fit

If the business required, say, a ledger where every order, payment, and inventory decrement must be transactionally consistent across dozens of tables with strict referential integrity and regulatory audit, a relational database (or MongoDB transactions with careful design) would be the more appropriate foundation.
