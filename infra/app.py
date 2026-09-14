import aws_cdk as cdk
from garage_stack import GarageMonitorStack

app = cdk.App()
GarageMonitorStack(app, "GarageMonitorStack")
app.synth()
