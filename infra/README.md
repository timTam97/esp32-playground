# Garage monitor AWS backend

Python CDK infrastructure and the Lambda handler used by the
[garage monitor firmware](../firmware/garage_monitor/README.md).

The current handler accepts a JPEG through a Lambda Function URL, stores it in
S3 under a UTC `YYYY-MM-DD/HH-MM-SS.jpg` key, asks Bedrock to classify the door,
and logs the result to CloudWatch. Every result with `status: open` sends an
SNS notification; historical door-state tracking is not implemented here.
Images expire after 30 days, and the bucket is retained if the stack is removed.

## Files

| File | Purpose |
| --- | --- |
| [app.py](app.py) | CDK application entry point. |
| [garage_stack.py](garage_stack.py) | Lambda, Function URL, S3, SNS, model configuration and permissions. |
| [lambda/handler.py](lambda/handler.py) | JPEG storage, Bedrock classification and SNS notification. |

## Commands and configuration

Run from the repository root, where [cdk.json](../cdk.json) selects this app.
The project uses uv for its Python dependencies and npx for the CDK CLI.

```sh
uv run npx cdk synth
uv run npx cdk diff
uv run npx cdk deploy --context email=you@email.com
```

The email context adds an SNS email subscription. The stack outputs
`FunctionUrl`, `SnsTopicArn`, and `ImageBucket`. Put the Function URL and Wi-Fi
credentials in the firmware's ignored `secrets.h`, created from its example.

The Bedrock model ID is configured in `garage_stack.py` and passed to Lambda
through `BEDROCK_MODEL_ID`; the bucket and topic use `S3_BUCKET` and
`SNS_TOPIC_ARN`. The current Function URL uses `FunctionUrlAuthType.NONE`.
Generated CDK output and local configuration are ignored by git.
