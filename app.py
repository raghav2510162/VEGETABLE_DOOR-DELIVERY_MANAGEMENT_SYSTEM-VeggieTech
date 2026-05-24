"""
VeggieTech Flask Application
=======================================
Architecture: Stateful C Backend via persistent subprocess.
 - The C backend (backend.exe) is started ONCE when Flask launches.
 - All business logic and data lives in C memory during the session.
 - On shutdown (Ctrl+C or process end), a SHUTDOWN command is sent
   to C which triggers file writes.
 - Email notifications are sent from Python using smtplib.
"""

import os
import sys
import subprocess
import threading
import atexit
import signal
import smtplib
from email.mime.text import MIMEText
from email.mime.multipart import MIMEMultipart
from flask import Flask, render_template, request, redirect, url_for, session, flash

import secrets

# ── App Setup ───────────────────────────────────────────────
app = Flask(__name__)
# Generate a random key on every start to ensure all users are logged out 
# if the server is killed and restarted.
app.secret_key = secrets.token_hex(16)

BASE_DIR = os.path.dirname(os.path.abspath(__file__))
BIN_DIR  = os.path.join(BASE_DIR, "bin")

def get_bin(name):
    ext = ".exe" if sys.platform == "win32" else ""
    return os.path.join(BIN_DIR, f"{name}{ext}")

# ── Email Configuration ─────────────────────────────────────
# Set these to your Gmail address and an App Password.
# To generate a Gmail App Password:
# 1. Enable 2-Factor Authentication on your Google account.
# 2. Go to: Google Account → Security → App Passwords.
# 3. Generate a password for "Mail" and paste it below.
EMAIL_SENDER   = "veggietech.shop@gmail.com"     
EMAIL_PASSWORD = "khbe govo okpr ofcp"   
EMAIL_ENABLED  = True  

def send_email(to_email, subject, body_html):
    """
    Sends an HTML email to the customer.
    This is called by Flask (Python) directly after the C backend
    confirms an event. The C backend has NO knowledge of emails;
    it only handles data logic. Python is the notification layer.
    """
    if not EMAIL_ENABLED:
        # Print to console instead when email is not configured
        print(f"[EMAIL STUB] To: {to_email} | Subject: {subject}")
        print(f"[EMAIL STUB] Body: {body_html[:200]}...")
        return True

    try:
        msg = MIMEMultipart("alternative")
        msg["Subject"] = subject
        msg["From"]    = EMAIL_SENDER
        msg["To"]      = to_email
        msg.attach(MIMEText(body_html, "html"))

        with smtplib.SMTP_SSL("smtp.gmail.com", 465) as server:
            server.login(EMAIL_SENDER, EMAIL_PASSWORD)
            server.sendmail(EMAIL_SENDER, to_email, msg.as_string())
        return True
    except Exception as e:
        print(f"[EMAIL ERROR] {e}")
        return False

def email_order_placed(name, email, order_id, total, items_html):
    subject = f"VeggieTech - Order {order_id} Placed Successfully!"
    body = f"""
    <h2 style="color:#2e7d32;">✅ Your Order is Confirmed!</h2>
    <p>Dear <b>{name}</b>,</p>
    <p>Your order <b>{order_id}</b> has been placed successfully.</p>
    {items_html}
    <p><b>Grand Total: Rs. {total}</b></p>
    <p>You will receive another email once your order is ready for delivery.</p>
    <br><p style="color:#999;">VeggieTech - Farm Fresh to Your Door</p>
    """
    send_email(email, subject, body)

def email_payment_confirmed(name, email, order_id, method, total):
    subject = f"VeggieTech - Payment Confirmed for {order_id}"
    body = f"""
    <h2 style="color:#2e7d32;">💳 Payment Received!</h2>
    <p>Dear <b>{name}</b>,</p>
    <p>We have received your payment of <b>Rs. {total}</b> via <b>{method.upper()}</b> 
       for order <b>{order_id}</b>.</p>
    <p>Our team is now preparing your order.</p>
    <br><p style="color:#999;">VeggieTech - Farm Fresh to Your Door</p>
    """
    send_email(email, subject, body)

def email_status_update(name, email, order_id, status, address=""):
    emoji_map = {
        "Packed":     "📦", "Dispatched": "🚚",
        "Delivered":  "✅", "Received":   "📥",
        "Cancelled":  "❌"
    }
    emoji = emoji_map.get(status, "🔔")
    subject = f"VeggieTech - Order {order_id} is now {status}"
    extra = ""
    if status == "Dispatched" and address:
        extra = f"<p>Your order is on its way to: <b>{address}</b></p><p>Estimated delivery: within 2 hours.</p>"
    elif status == "Delivered":
        extra = "<p>We hope you enjoy your fresh vegetables! 🥦🍅</p>"
    body = f"""
    <h2>{emoji} Order Update: {status}</h2>
    <p>Dear <b>{name}</b>,</p>
    <p>Your order <b>{order_id}</b> status has been updated to: <b>{status}</b>.</p>
    {extra}
    <br><p style="color:#999;">VeggieTech - Farm Fresh to Your Door</p>
    """
    send_email(email, subject, body)

# ── Persistent C Backend Process ────────────────────────────
_backend_proc  = None
_backend_lock  = threading.Lock()

def start_backend():
    """Starts backend.exe as a persistent subprocess."""
    global _backend_proc
    exe = get_bin("backend")
    if not os.path.exists(exe):
        print(f"[ERROR] Backend binary not found: {exe}")
        print("[ERROR] Please compile c_programs/backend.c first:")
        print(f"        gcc c_programs/backend.c -o {exe}")
        sys.exit(1)
    _backend_proc = subprocess.Popen(
        [exe],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        bufsize=1,           # line-buffered
        cwd=BASE_DIR
    )
    print(f"[VeggieTech] C backend started (PID {_backend_proc.pid})")

def send_command(cmd_line):
    """
    Sends one command line to the C backend and collects the response
    until a line containing only 'END' is received.
    Returns the raw response as a string.
    Thread-safe via lock.
    """
    with _backend_lock:
        if _backend_proc is None or _backend_proc.poll() is not None:
            return "ERROR:Backend not running\nEND"
        try:
            _backend_proc.stdin.write(cmd_line + "\n")
            _backend_proc.stdin.flush()
            lines = []
            while True:
                line = _backend_proc.stdout.readline()
                if not line:
                    break
                line = line.rstrip("\n\r")
                if line == "END":
                    break
                lines.append(line)
            return "\n".join(lines)
        except Exception as e:
            return f"ERROR:{e}"

def parse_response(output):
    """
    Parses the KEY:VALUE response from C into a Python dict.
    Lines starting with ITEM:, VEG:, ORDER: go into list keys.
    """
    parsed = {"items_list": [], "veg_list": [], "orders_list": []}
    for line in output.splitlines():
        line = line.strip()
        if line.startswith("ITEM:"):
            parts = line[5:].split("|")
            if len(parts) == 5:
                parsed["items_list"].append({
                    "name": parts[0], "qty": parts[1],
                    "unit": parts[2], "price": parts[3],
                    "amount": f"{float(parts[4]):.2f}"
                })
        elif line.startswith("VEG:"):
            parts = line[4:].split("|")
            if len(parts) == 4:
                stock = float(parts[2])
                parsed["veg_list"].append({
                    "name":     parts[0],
                    "raw_name": parts[0],
                    "price":    int(parts[1]),
                    "stock":    stock,
                    "unit":     parts[3],
                    "status":   "available" if stock >= 10 else "low" if stock > 0 else "out"
                })
        elif line.startswith("ORDER:"):
            parts = line[6:].split("|")
            if len(parts) >= 6:
                parsed["orders_list"].append({
                    "id": parts[0], "name": parts[1],
                    "phone": parts[2], "payment": parts[3],
                    "total": parts[4], "status": parts[5],
                    "email": parts[6] if len(parts) > 6 else ""
                })
        elif ":" in line:
            key, _, val = line.partition(":")
            parsed[key.strip()] = val.strip()
    return parsed

def load_inventory():
    """Asks C backend for current inventory. Used by multiple routes."""
    out = send_command("GET_INVENTORY")
    return parse_response(out).get("veg_list", [])

# ── Shutdown Handler ─────────────────────────────────────────
def shutdown_backend():
    """Called on Flask exit. Sends SHUTDOWN to C so it saves data."""
    global _backend_proc
    if _backend_proc is None:
        return
        
    proc = _backend_proc
    _backend_proc = None # Mark as handled immediately to prevent recursion
    
    if proc.poll() is None:
        print("\n[VeggieTech] Sending SHUTDOWN to C backend — saving data...")
        try:
            # We use a raw write here to avoid any lock-contention during shutdown
            proc.stdin.write("SHUTDOWN\n")
            proc.stdin.flush()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.terminate()
        except:
            try:
                proc.terminate()
            except:
                pass
        print("[VeggieTech] Data saved. Goodbye.")

if not hasattr(app, '_backend_initialized'):
    atexit.register(shutdown_backend)
    app._backend_initialized = True

def handle_sigint(sig, frame):
    shutdown_backend()
    sys.exit(0)

signal.signal(signal.SIGINT, handle_sigint)
if hasattr(signal, "SIGTERM"):
    signal.signal(signal.SIGTERM, handle_sigint)

# ── Routes ───────────────────────────────────────────────────

@app.route("/")
def index():
    """Home page showing today's vegetable availability."""
    inventory = load_inventory()
    return render_template("index.html", inventory=inventory)

@app.route("/login", methods=["GET", "POST"])
def login():
    """Admin authentication."""
    if request.method == "POST":
        u = request.form.get("username", "")
        p = request.form.get("password", "")
        out = send_command(f'LOGIN "{u}" "{p}"')
        data = parse_response(out)
        if data.get("SUCCESS") is not None or "SUCCESS" in out:
            session["admin"] = u
            flash(f"Welcome, {u}!", "success")
            return redirect(url_for("admin_dashboard"))
        flash("Invalid credentials.", "error")
    return render_template("login.html")

@app.route("/logout")
def logout():
    session.clear()
    return redirect(url_for("index"))

@app.route("/admin")
def admin_dashboard():
    """Admin dashboard: inventory + order management."""
    if "admin" not in session:
        return redirect(url_for("login"))
    
    out = send_command("GET_ORDERS_ALL")
    data = parse_response(out)
    all_orders = data.get("orders_list", [])

    orders = []
    archived_orders = []
    
    for o in all_orders:
        if o["status"] in ["Delivered", "Cancelled"]:
            archived_orders.append(o)
        else:
            orders.append(o)

    inventory = load_inventory()
    return render_template("admin.html", admin=session["admin"],
                           orders=orders, archived_orders=archived_orders, inventory=inventory)

@app.route("/admin/add_vegetable", methods=["POST"])
def add_vegetable():
    """Admin: add a new vegetable to inventory (in C memory)."""
    if "admin" not in session:
        return redirect(url_for("login"))
    name  = request.form.get("veg_name", "").strip().title().replace(" ", "_")
    price = request.form.get("veg_price", "0")
    stock = request.form.get("veg_stock", "0")
    unit  = request.form.get("veg_unit",  "kg")
    out = send_command(f'ADD_VEGETABLE "{name}" {price} {stock} "{unit}"')
    if "SUCCESS" in out:
        flash(f"Added {name.replace('_',' ')} to inventory.", "success")
    else:
        data = parse_response(out)
        flash(data.get("ERROR", "Failed to add vegetable."), "error")
    return redirect(url_for("admin_dashboard"))

@app.route("/admin/update_inventory", methods=["POST"])
def update_inventory():
    """Admin: bulk update prices and stock for all vegetables."""
    if "admin" not in session:
        return redirect(url_for("login"))
    inventory = load_inventory()
    args = []
    for v in inventory:
        price = request.form.get(f"price_{v['raw_name']}", str(v['price']))
        stock = request.form.get(f"stock_{v['raw_name']}", str(v['stock']))
        args += [price, stock]
    cmd = "UPDATE_INVENTORY " + " ".join(args)
    send_command(cmd)
    flash("Inventory updated in memory.", "success")
    return redirect(url_for("admin_dashboard"))

@app.route("/admin/update_status", methods=["POST"])
def update_status():
    """Admin: update order fulfillment status and send customer email notification."""
    if "admin" not in session:
        return redirect(url_for("login"))
    order_id   = request.form.get("order_id")
    new_status = request.form.get("status")
    cust_email = request.form.get("cust_email", "")

    out  = send_command(f'UPDATE_STATUS "{order_id}" "{new_status}"')
    data = parse_response(out)

    if "SUCCESS" in out:
        flash(f"Order {order_id} updated to {new_status}.", "success")
        # Send email notification to customer
        name    = data.get("NAME", "Customer")
        address = data.get("ADDRESS", "")
        total   = data.get("TOTAL", "")
        if cust_email:
            email_status_update(name, cust_email, order_id, new_status, address)
    else:
        flash(data.get("ERROR", "Update failed."), "error")
    return redirect(url_for("admin_dashboard"))

@app.route("/order", methods=["GET", "POST"])
def order():
    """Customer order placement with live bill total."""
    inventory = load_inventory()
    if request.method == "POST":
        name    = request.form.get("name", "").strip()
        phone   = request.form.get("phone", "").strip()
        address = request.form.get("address", "").strip().replace("\n", " ").replace("|", " ")
        payment = request.form.get("payment", "cod").lower()
        email   = request.form.get("email", "").strip()

        # Build quantity args in inventory order
        qtys = [request.form.get(v["raw_name"], "0") for v in inventory]

        # Escape address (replace spaces with underscore trick or use quotes)
        safe_address = address.replace('"', "'")
        cmd = (f'PLACE_ORDER "{name}" "{phone}" "{email}" "{safe_address}" "{payment}" '
               + " ".join(qtys))
        out  = send_command(cmd)
        data = parse_response(out)

        if "SUCCESS" in out:
            order_id = data.get("ORDER_ID", "")
            total    = data.get("TOTAL", "0")
            # Build items HTML for email
            items_html = "<table border='1' cellpadding='6'><tr><th>Item</th><th>Qty</th><th>Price</th><th>Amount</th></tr>"
            for item in data.get("items_list", []):
                items_html += f"<tr><td>{item['name']}</td><td>{item['qty']} {item['unit']}</td><td>Rs.{item['price']}</td><td>Rs.{item['amount']}</td></tr>"
            items_html += f"</table><p>Delivery charge: Rs. 15.00</p>"

            # Removed 'Order Placed' email. Will only email upon Payment & Update.
            # if email:
            #     email_order_placed(name, email, order_id, total, items_html)

            # Store order details for success page
            data["NAME"]    = name
            data["PHONE"]   = phone
            data["ADDRESS"] = address
            data["PAYMENT"] = payment
            return render_template("order_success.html", data=data)
        else:
            flash(data.get("ERROR", "Could not place order."), "error")
    return render_template("order.html", inventory=inventory)

@app.route("/bill", methods=["GET", "POST"])
def bill():
    """Customer bill view. Requires order_id + phone passed via URL or form (from order_success page)."""
    order_id = request.args.get("order_id") or request.form.get("order_id")
    phone    = request.args.get("phone")     or request.form.get("phone")

    if order_id and phone:
        out  = send_command(f'GET_ORDER "{order_id}" "{phone}"')
        data = parse_response(out)
        if "SUCCESS" in out:
            p_out  = send_command(f'PAY_STATUS "{order_id}" "{phone}"')
            p_data = parse_response(p_out)
            data["PAY_STATUS"] = p_data.get("STATUS", "Unpaid")
            return render_template("bill.html", data=data)
        flash("Order not found. Please check your Order ID and phone number.", "error")

    # No order details — bill is sent by email, no lookup form needed
    flash("Your bill was sent to your email when you placed the order.", "success")
    return redirect(url_for("index"))

@app.route("/payment", methods=["POST"])
def payment():
    """Process payment and send confirmation email."""
    order_id = request.form.get("order_id")
    phone    = request.form.get("phone")
    method   = request.form.get("payment_method", "cod").lower()
    out  = send_command(f'PAY_ORDER "{order_id}" "{phone}" "{method}"')
    data = parse_response(out)

    if "SUCCESS" in out:
        name  = data.get("NAME", "Customer")
        total = data.get("TOTAL", "0")
        
        # Fetch full order to get email and for the success page
        o_out  = send_command(f'GET_ORDER "{order_id}" "{phone}"')
        o_data = parse_response(o_out)
        email  = o_data.get("EMAIL", "")

        if email:
            email_payment_confirmed(name, email, order_id, method, total)
            
        return render_template("payment_success.html", data=data, order_data=o_data)
    flash("Payment processing failed. Please try again.", "error")
    return redirect(url_for("bill", order_id=order_id, phone=phone))

@app.route("/cancel_order", methods=["POST"])
def cancel_order():
    """Customer cancels an unpaid order before checking out, releasing inventory."""
    order_id = request.form.get("order_id")
    if order_id:
        out = send_command(f'UPDATE_STATUS "{order_id}" "Cancelled"')
        if "SUCCESS" in out:
            flash("Order was safely cancelled and items were restocked.", "success")
        else:
            flash("Error cancelling order.", "error")
    return redirect(url_for("index"))

# ── Entry Point ──────────────────────────────────────────────
if __name__ == "__main__":
    start_backend()
    # Use threaded=False to avoid multi-thread lock issues with the pipe
    app.run(debug=False, port=5000, threaded=False)
