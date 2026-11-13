// Stands in for the runtime module when worker.js is loaded under Node.
export class DurableObject {
  constructor(ctx, env) {
    this.ctx = ctx;
    this.env = env;
  }
}
